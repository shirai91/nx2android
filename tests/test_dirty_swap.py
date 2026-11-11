"""nvn_dirty_swap replaces a tracked range's pages while another thread writes them, and no write is lost."""
from pathlib import Path
import subprocess
import tempfile
import unittest

import sys; sys.path.insert(0, str(Path(__file__).resolve().parent))
import host_cc   # noqa: E402


class DirtySwap(unittest.TestCase):
    @unittest.skipUnless(host_cc.posix(), 'a POSIX host C compiler is required (<sys/mman.h>, sigaction, mkstemp)')
    def test_swap_under_writes(self):
        dm = Path(__file__).resolve().parents[1] / 'nxnative/dm'
        dirty = (dm / 'gfx/nvn/nvn_dirty.c').read_text(encoding='utf-8')
        dirty = '\n'.join(s for s in dirty.splitlines() if not s.startswith('#include'))
        harness = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;
static void mach_timebase_info(mach_timebase_info_data_t *t) { t->numer = t->denom = 1; }
static uint64_t mach_absolute_time(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define die(...) do { LOG(__VA_ARGS__); abort(); } while (0)
static int (*fault_hook)(void *, int, void *);
static void (*write_hook)(void *, size_t);
static void dm_fault_hook_register(int (*fn)(void *, int, void *)) { fault_hook = fn; }
static void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { write_hook = fn; }
static int fail_ro;   // 1: the next mprotect to read-only fails
static int checked_mprotect(void *a, size_t n, int prot) { if (fail_ro && prot == PROT_READ) { fail_ro = 0; errno = ENOMEM; return -1; } return mprotect(a, n, prot); }
#define mprotect checked_mprotect
DIRTY
static void handle_fault(int sig, siginfo_t *info, void *uc) { (void)sig; if (!fault_hook(info->si_addr, 1, uc)) _exit(99); }   // not the tracker's: a lost or stuck write
static uint8_t *mem; static size_t pg, np, swapped; static int fd, fd2, fills, fail_at = -1; static atomic_int stop; static uint32_t *last;
static int fill(void *at, size_t n, size_t off, void *ctx) {   // as gfx_vk_core.c's zc_fill: the old pages into the new ones, the new ones mapped over them read-only
    (void)ctx; if (fills++ == fail_at) return -6;
    uint8_t *tmp = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)off); if (tmp == MAP_FAILED) return -5;
    memcpy(tmp, at, n); munmap(tmp, n);
    return mmap(at, n, PROT_READ, MAP_SHARED | MAP_FIXED, fd, (off_t)off) == at ? 0 : -6;
}
static int fill_back(void *at, size_t n, size_t off, void *ctx) {   // as gfx_vk_core.c's zc_anon_fill: other new pages (a second file here), mapped over them read-only
    (void)ctx; uint8_t *tmp = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, (off_t)off); if (tmp == MAP_FAILED) return -5;
    memcpy(tmp, at, n); munmap(tmp, n);
    return mmap(at, n, PROT_READ, MAP_SHARED | MAP_FIXED, fd2, (off_t)off) == at ? 0 : -6;
}
static void *writer(void *a) {   // a game thread: a new value into a pseudo-random page, over and over
    (void)a; uint32_t v = 0, x = 12345;
    while (!atomic_load(&stop)) { x = x * 1103515245u + 12345u; size_t p = (x >> 8) % np; ((volatile uint32_t *)(mem + p * pg))[1] = ++v; last[p] = v; }
    return NULL;
}
static void check(const char *what) {
    for (size_t p = 0; p < np; p++) {
        const uint32_t *w = (const uint32_t *)(mem + p * pg);
        if (w[0] != (uint32_t)p || (last[p] && w[1] != last[p])) { fprintf(stderr, "%s: page %zu holds %u %u, want %zu %u\n", what, p, w[0], w[1], p, last[p]); _exit(98); }
    }
}
static void run(size_t len, int fail, int ro_fails, int back, int *rc_out) {   // back: the slices swapped before a failure are swapped again onto other pages (zc_unplace)
    np = len / pg; fills = 0; fail_at = fail; atomic_store(&stop, 0);
    mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); assert(mem != MAP_FAILED);
    last = calloc(np, sizeof *last); for (size_t p = 0; p < np; p++) ((uint32_t *)(mem + p * pg))[0] = (uint32_t)p;
    char path[] = "/tmp/nx2a-swap-XXXXXX"; fd = mkstemp(path); assert(fd >= 0); unlink(path); assert(ftruncate(fd, (off_t)len) == 0);
    char path2[] = "/tmp/nx2a-swap-XXXXXX"; fd2 = mkstemp(path2); assert(fd2 >= 0); unlink(path2); assert(ftruncate(fd2, (off_t)len) == 0);
    nvn_dirty_add_range(mem, len); assert(nvn_dirty_quiet(mem, len) == 0);   // no host write prepared: go ahead
    nvn_dirty_protect(mem + 100 * pg, 64 * pg);   // a texture's storage, protected as the replay leaves it
    pthread_t t; assert(pthread_create(&t, NULL, writer, NULL) == 0);
    usleep(2000);
    fail_ro = ro_fails; *rc_out = nvn_dirty_swap(mem, len, fill, NULL, &swapped);
    if (back && swapped) { size_t again = 0; assert(nvn_dirty_swap(mem, swapped, fill_back, NULL, &again) == 0 && again == swapped); }
    usleep(20000);   // writes after the swap: writable pages stay writable, protected ones fault and are let through
    atomic_store(&stop, 1); pthread_join(t, NULL);
    check("after the swap");
}
int main(void) {
    pg = (size_t)getpagesize();
    struct sigaction sa = {0}; sa.sa_sigaction = handle_fault; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    assert(sigaction(SIGSEGV, &sa, NULL) == 0 && sigaction(SIGBUS, &sa, NULL) == 0);
    int rc;
    // 40 MB: three slices; every page's last write is there, and the pages are the new ones (the file's)
    run(40u << 20, -1, 0, 0, &rc); assert(rc == 0 && fills == 3 && swapped == 40u << 20);
    uint8_t *view = mmap(NULL, 40u << 20, PROT_READ, MAP_SHARED, fd, 0); assert(view != MAP_FAILED && memcmp(view, mem, 40u << 20) == 0);
    for (size_t p = 0; p < np; p += 97) ((volatile uint32_t *)(mem + p * pg))[2] = 7;   // every page writable (or let through)
    nvn_dirty_remove_range(mem);
    // a fill failing in the second slice: the first stays swapped (reported as done), the rest keeps its old pages; no write lost, every page writable
    run(40u << 20, 1, 0, 0, &rc); assert(rc == -6 && fills == 2 && swapped == 16u << 20);
    for (size_t p = 0; p < np; p++) ((volatile uint32_t *)(mem + p * pg))[2] = 7;
    nvn_dirty_remove_range(mem);
    // the same, with the swapped slice put on other pages before its buffer goes (gfx_vk_core.c zc_unplace): no write lost, and none of it on the first file's pages
    run(40u << 20, 1, 0, 1, &rc); assert(rc == -6 && swapped == 16u << 20);
    uint8_t *v2 = mmap(NULL, 16u << 20, PROT_READ, MAP_SHARED, fd2, 0); assert(v2 != MAP_FAILED && memcmp(v2, mem, 16u << 20) == 0); munmap(v2, 16u << 20);
    for (size_t p = 0; p < np; p++) ((volatile uint32_t *)(mem + p * pg))[2] = 7;
    nvn_dirty_remove_range(mem);
    // a range the host prepared a write into just now (a file read into the pool): nothing swapped, try later (2), not a failure; the pages as they were
    mem = mmap(NULL, 8u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); assert(mem != MAP_FAILED);
    nvn_dirty_add_range(mem, 8u << 20); write_hook(mem + 3 * pg, 2 * pg); fills = 0;
    assert(nvn_dirty_quiet(mem, 8u << 20) == 2 && nvn_dirty_quiet(mem + pg, pg) == 2);   // asked before any buffer is taken (gfx_vk_core.c pool_zero_copy)
    assert(nvn_dirty_swap(mem, 8u << 20, fill, NULL, &swapped) == 2 && fills == 0 && swapped == 0);
    ((volatile uint32_t *)mem)[0] = 1;
    nvn_dirty_remove_range(mem);
    // mprotect to read-only failing: nothing swapped in that slice, the pages writable again
    run(8u << 20, -1, 1, 0, &rc); assert(rc == -1 && fills == 0 && swapped == 0);
    for (size_t p = 0; p < np; p++) ((volatile uint32_t *)(mem + p * pg))[2] = 7;
    // spans the tracker does not hold, or not whole pages: nothing done
    uint8_t *other = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(nvn_dirty_swap(other, pg, fill, NULL, NULL) == 1 && nvn_dirty_swap(mem + 8, pg, fill, NULL, NULL) == -1);
    assert(nvn_dirty_quiet(other, pg) == 1 && nvn_dirty_quiet(mem + 8, pg) == -1);
    return 0;
}
'''.replace('DIRTY', dirty)
        with tempfile.TemporaryDirectory(prefix='nx2a-dirty-swap-') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-pthread', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-parameter', str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == '__main__':
    unittest.main()
