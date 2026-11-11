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
        core = (dm / 'android/gfx_vk_core.c').read_text(encoding='utf-8')   # gfx_vk_core.c's own fill, for a lazy pool with pools inside it
        start = core.index('typedef struct { const uint64_t *r; int n; } zcin_t;')
        zc = core[start:core.index('\n}\n', core.index('static int zc_fill(', start)) + 3]
        harness = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
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
// 1: this thread's next page-state exchange (nvn_dirty_swap taking its next slice) first lets a retirement run (retire_stage), as t_dirty_retire_race stages one in nvn_dirty_protect
static _Thread_local int retire_at_cas;
static void retire_stage(void);
#undef atomic_compare_exchange_strong
#define atomic_compare_exchange_strong(o, e, d) ((retire_at_cas ? retire_stage() : (void)0), atomic_compare_exchange_strong_explicit(o, e, d, memory_order_seq_cst, memory_order_seq_cst))
#define want(c, ...) do { if (!(c)) { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); _exit(97); } } while (0)
DIRTY
static int fail_fixed = -1;   // the n-th fixed mapping in gfx_vk_core.c's zc_fill fails (0: the next one)
static void *checked_mmap(void *a, size_t n, int prot, int flags, int f, off_t off) { if ((flags & MAP_FIXED) && fail_fixed >= 0 && fail_fixed-- == 0) { errno = ENOMEM; return MAP_FAILED; } return mmap(a, n, prot, flags, f, off); }
#define mmap checked_mmap
ZCFILL
#undef mmap
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
static int zc_anon_fill(void *at, size_t n, size_t off, void *ctx) {   // as gfx_vk_core.c's (no mremap here), onto the second file's pages at the same offsets, so the test sees where they went
    (void)off;
    for (uint64_t a = (uintptr_t)at, e = a + n, lo, hi; a < e; a = hi) {
        zc_next_in(ctx, a, e, &lo, &hi); if (lo == a) continue;
        off_t o = (off_t)(a - (uintptr_t)mem); uint8_t *tmp = mmap(NULL, lo - a, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, o); if (tmp == MAP_FAILED) return -5;
        memcpy(tmp, (void *)(uintptr_t)a, lo - a); munmap(tmp, lo - a);
        if (mmap((void *)(uintptr_t)a, lo - a, PROT_READ, MAP_SHARED | MAP_FIXED, fd2, o) != (void *)(uintptr_t)a) return -6;
    }
    return 0;
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
static int fdi; static uint64_t nr[4];
static int nest_inner(size_t p) { uint64_t a = (uintptr_t)mem + p * pg; return (a >= nr[0] && a < nr[1]) || (a >= nr[2] && a < nr[3]); }
static void nest_run(int fail, int *rc) {   // a lazy pool with two pools inside it on their own file's pages (fdi), one across the first slice's end, given its buffer (fd) by
    size_t len = 40u << 20; np = len / pg; atomic_store(&stop, 0);   // gfx_vk_core.c's zc_fill while a game thread writes; fail: its n-th fixed mapping fails
    mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); assert(mem != MAP_FAILED);
    free(last); last = calloc(np, sizeof *last); for (size_t p = 0; p < np; p++) ((uint32_t *)(mem + p * pg))[0] = (uint32_t)p;
    char p1[] = "/tmp/nx2a-swap-XXXXXX", p2[] = "/tmp/nx2a-swap-XXXXXX", p3[] = "/tmp/nx2a-swap-XXXXXX";
    fd = mkstemp(p1); fd2 = mkstemp(p2); fdi = mkstemp(p3); assert(fd >= 0 && fd2 >= 0 && fdi >= 0); unlink(p1); unlink(p2); unlink(p3);
    assert(!ftruncate(fd, (off_t)len) && !ftruncate(fd2, (off_t)len) && !ftruncate(fdi, (off_t)len));
    uint64_t m = (uintptr_t)mem; nr[0] = m + 10 * pg; nr[1] = m + 30 * pg; nr[2] = m + (16u << 20) - 4 * pg; nr[3] = m + (16u << 20) + 8 * pg;
    for (int i = 0; i < 2; i++) {   // the inner pools' own pages, as pool_zero_copy made them before the pool around them got its buffer
        size_t n = nr[2 * i + 1] - nr[2 * i]; off_t o = (off_t)(nr[2 * i] - m); void *at = (void *)(uintptr_t)nr[2 * i];
        uint8_t *t = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fdi, o); assert(t != MAP_FAILED); memcpy(t, at, n); munmap(t, n);
        assert(mmap(at, n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fdi, o) == at);
    }
    zcin_t in = {nr, 2}; zcfill_t f = {fd, 0, NULL, PROT_READ, &in};
    nvn_dirty_add_range(mem, len);
    pthread_t t; assert(pthread_create(&t, NULL, writer, NULL) == 0);
    usleep(2000);
    fail_fixed = fail; *rc = nvn_dirty_swap(mem, len, zc_fill, &f, &swapped); fail_fixed = -1;
    usleep(20000);
    atomic_store(&stop, 1); pthread_join(t, NULL);
    check("after the swap around the pools inside");
}
static uint32_t nest_word(int f, size_t p) { uint32_t v; assert(pread(f, &v, 4, (off_t)(p * pg + 12)) == 4); return v; }   // word 3 of page p in a file
// A pool retired while its swap runs (hle_nvn.c: nvn_dirty_remove_range, then mtl_pool_del, which waits for pool_back): fill_retire arms retire_stage in the first slice,
// which retires the range on another thread as the swap takes its second slice and gives it 100 ms; the same thread then registers a new pool elsewhere (fresh), which
// takes the freed slot with prep 0, so a swap that went on found no "written lately" to stop it and worked from fresh's base and state array. The second slice's fill
// starts a host write into its pages (prep_write, then read() from /dev/zero, as a file read into the pool) and gives it 50 ms
static atomic_int retired, read_done; static pthread_t retirer, rdr; static int rdr_on, read_err; static ssize_t read_got; static uint8_t *fresh; enum { FRESH = 4u << 20 };
static void *retire_thread(void *a) { (void)a; nvn_dirty_remove_range(mem); nvn_dirty_add_range(fresh, FRESH); atomic_store(&retired, 1); return NULL; }
static void retire_stage(void) {
    retire_at_cas = 0; assert(pthread_create(&retirer, NULL, retire_thread, NULL) == 0);
    for (int i = 0; i < 100 && !atomic_load(&retired); i++) usleep(1000);   // over at once, unless the swap holds g_slot_mu
}
static void *reader(void *a) { int z = open("/dev/zero", O_RDONLY); write_hook(a, 16); read_got = read(z, a, 16); read_err = errno; close(z); atomic_store(&read_done, 1); return NULL; }
static int fill_retire(void *at, size_t n, size_t off, void *ctx) {
    if (fills == 0) retire_at_cas = 1;   // the swap's next page-state exchange takes the second slice
    if (fills == 1) { rdr_on = 1; assert(pthread_create(&rdr, NULL, reader, (uint8_t *)at + 64) == 0); for (int i = 0; i < 50 && !atomic_load(&read_done); i++) usleep(1000); }   // (bytes 64-79: not the words check reads)
    return fill(at, n, off, ctx);
}
static void retire_run(int *rc, range_t **slot) {
    size_t len = 40u << 20; np = len / pg; fills = 0; fail_at = -1; rdr_on = 0; atomic_store(&stop, 0); atomic_store(&retired, 0); atomic_store(&read_done, 0);
    mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); assert(mem != MAP_FAILED);
    free(last); last = calloc(np, sizeof *last); for (size_t p = 0; p < np; p++) ((uint32_t *)(mem + p * pg))[0] = (uint32_t)p;
    char p1[] = "/tmp/nx2a-swap-XXXXXX", p2[] = "/tmp/nx2a-swap-XXXXXX"; fd = mkstemp(p1); fd2 = mkstemp(p2); assert(fd >= 0 && fd2 >= 0); unlink(p1); unlink(p2);
    assert(!ftruncate(fd, (off_t)len) && !ftruncate(fd2, (off_t)len));
    fresh = mmap(NULL, FRESH, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); assert(fresh != MAP_FAILED);
    nvn_dirty_add_range(mem, len); nvn_dirty_protect(mem + 100 * pg, 64 * pg); *slot = find_span((uintptr_t)mem, len);
    pthread_t t; assert(pthread_create(&t, NULL, writer, NULL) == 0);
    usleep(2000);
    *rc = nvn_dirty_swap(mem, len, fill_retire, NULL, &swapped);
    pthread_join(retirer, NULL); if (rdr_on) pthread_join(rdr, NULL);
    usleep(20000);   // writes after the swap, into a pool no range holds now: every page must be writable
    atomic_store(&stop, 1); pthread_join(t, NULL);
    check("after a retirement during the swap");
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
    // pools inside a lazy pool (gfx_vk_core.c pool_back, zcin_t): the buffer's pages go only around them, and the buffer gets their bytes too; a write afterwards lands
    // on the inner file's page inside them and on the buffer's page elsewhere
    nest_run(-1, &rc); assert(rc == 0 && swapped == 40u << 20);
    for (size_t p = 0; p < np; p++) {
        uint32_t v; assert(pread(fd, &v, 4, (off_t)(p * pg)) == 4 && v == (uint32_t)p);
        uint32_t w = 0xab000000u + (uint32_t)p; ((volatile uint32_t *)(mem + p * pg))[3] = w;
        if (nest_inner(p)) assert(nest_word(fdi, p) == w && nest_word(fd, p) != w); else assert(nest_word(fd, p) == w);
    }
    nvn_dirty_remove_range(mem);
    // the second fixed mapping of the first slice failing: the slice is reported failed, the piece mapped before it goes back on other pages (the second file's here,
    // anonymous memory in gfx_vk_core.c) so none of the slice stays on the buffer's pages, and the pool inside keeps its own; no write lost
    nest_run(1, &rc); assert(rc == -6 && swapped == 0);
    for (size_t p = 0; p < (16u << 20) / pg; p++) {
        uint32_t w = 0xcd000000u + (uint32_t)p; ((volatile uint32_t *)(mem + p * pg))[3] = w;
        assert(nest_word(fd, p) != w);
        if (nest_inner(p)) assert(nest_word(fdi, p) == w);
        else if ((uintptr_t)mem + p * pg < nr[0]) assert(nest_word(fd2, p) == w);
        else assert(nest_word(fd2, p) != w);   // past the failed piece: the old anonymous pages
    }
    nvn_dirty_remove_range(mem);
    // the range retired while the swap runs, its slot then taken by a new pool elsewhere: the second slice, taken before the retirement could finish, is swapped, and
    // the retirement waits for it; the third finds another registration in the slot and is not swapped (1: the tracker no longer holds the span; done = the two
    // slices). The host write into the second slice waits for its BUSY pages and lands (no EFAULT), no game write faults with no owner (the writer ran throughout: no
    // crash, no lost write), the new pool's pages are untouched, and the put-back (gfx_vk_core.c zc_unplace) finds no range either, which leaves the swapped slices to
    // mtl_pool_del's copy_back (vkpool_t left)
    range_t *slot; retire_run(&rc, &slot);
    want(rc == 1 && fills == 2 && swapped == 32u << 20, "retired during the swap: rc %d, %d slices filled, %zu bytes swapped (want 1, 2, %u)", rc, fills, swapped, 32u << 20);
    want(atomic_load(&retired), "the retirement did not finish");
    want(read_got == 16, "host write into the second slice: %zd bytes, errno %d (want 16)", read_got, read_got < 0 ? read_err : 0);
    { size_t again = 1; want(nvn_dirty_quiet(mem, 40u << 20) == 1 && nvn_dirty_swap(mem, swapped, fill_back, NULL, &again) == 1 && again == 0, "a retired range still answers"); }
    for (size_t p = 0; p < np; p++) ((volatile uint32_t *)(mem + p * pg))[2] = 7;
    want(find_span((uintptr_t)fresh, FRESH) == slot, "the staging: the new pool did not take the retired range's slot");
    for (size_t p = 0; p < FRESH / pg; p++) want(atomic_load(&slot->state[p]) == PG_NONE, "the new pool's page %zu: state %d (want none)", p, atomic_load(&slot->state[p]));
    want(nvn_dirty_quiet(fresh, FRESH) == 0, "the new pool: not quiet");
    for (size_t p = 0; p < FRESH / pg; p++) fresh[p * pg] = 1;
    nvn_dirty_remove_range(fresh);
    return 0;
}
'''.replace('DIRTY', dirty).replace('ZCFILL', zc)
        with tempfile.TemporaryDirectory(prefix='nx2a-dirty-swap-') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-pthread', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-parameter', str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == '__main__':
    unittest.main()
