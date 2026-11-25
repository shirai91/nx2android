"""nvn_dirty.c's lock-free readers against pool registration and retirement (ports of seedds/nxnative's t_dirty_init, t_dirty_reuse, t_dirty_retire_race and the
t_dirty_slots / t_dirty_race cases of b65b9b0f, a2c731e3 and a3d58526): the hooks exist before the first pool, a stamp from an older pool over the same memory counts as written,
a write on a pool being retired or reused never crashes, protection never lands on a retired pool, a read-only page that is not the tracker's is handed on, and overlapping pools
keep their first-match lookups. Each case runs in its own process: a lost write ends it with 99 (a fault with no owner) or a signal."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

import sys; sys.path.insert(0, str(Path(__file__).resolve().parent))
import host_cc   # noqa: E402

DM = Path(__file__).resolve().parents[1] / 'nxnative/dm'
HARNESS = r'''
#define _GNU_SOURCE
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
static void dm_guest_write_prepare(void *a, size_t n) { if (write_hook) write_hook(a, n); }
// 1: this thread's next page-state exchange first lets a retirement run (retire_stage): nvn_dirty_protect's first one comes right after its lookup (t_dirty_retire_race)
static _Thread_local int stage_at_cas;
static void retire_stage(void);
#undef atomic_compare_exchange_strong
#define atomic_compare_exchange_strong(o, e, d) ((stage_at_cas ? retire_stage() : (void)0), atomic_compare_exchange_strong_explicit(o, e, d, memory_order_seq_cst, memory_order_seq_cst))
static int g_fail;
#define EXPECT(name, want, got) do { long long w_ = (long long)(want), g_ = (long long)(got); if (w_ != g_) { fprintf(stderr, "%s: expected %lld, got %lld\n", name, w_, g_); g_fail++; } } while (0)
DIRTY
static void handle_fault(int sig, siginfo_t *info, void *uc) { (void)sig; if (!fault_hook || !fault_hook(info->si_addr, 1, uc)) _exit(99); }   // not the tracker's: a lost or stuck write
// pg: the host page size, 4 KB (Linux, Android, an x86_64 build under Rosetta on the Mac) or 16 KB (the Mac). sub: a quarter page, the step of the pools'
// sub-page starts and lengths, so that neighbours share host pages and no length or offset is 0 or negative at either size (a fixed 4096 was 0 at 4 KB pages)
static size_t pg, sub;
static uint8_t *page_map(size_t n) { uint8_t *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); if (p == MAP_FAILED) { perror("mmap"); _exit(2); } return p; }

// de2018e0: geometry and both hooks are set when the program loads, before any pool; then eight threads register their first pools at once while another prepares host writes
enum { N_INIT = 8 };
static _Atomic int ready, stop;
static uint8_t *init_pages[N_INIT];
static void go(void) { atomic_fetch_add(&ready, 1); while (atomic_load(&ready) < N_INIT + 1) {} }
static void *pre_writer(void *arg) { uint8_t *spare = arg; go(); while (!atomic_load(&stop)) { dm_guest_write_prepare(spare, 64); spare[0]++; } return NULL; }
static void *first_pool(void *arg) {
    uint8_t *p = init_pages[(intptr_t)arg]; go();
    nvn_dirty_add_range(p, pg);
    EXPECT("tracked", 1, nvn_dirty_tracked(p, pg));
    uint64_t stamp = nvn_dirty_now(); nvn_dirty_protect(p, pg);
    EXPECT("clean once protected", 0, nvn_dirty_since(p, pg, stamp));
    ((volatile uint8_t *)p)[pg - 1] = 0x5a;   // faults: the fault hook unprotects and stamps the page
    EXPECT("direct write dirty", 1, nvn_dirty_since(p, pg, stamp));
    stamp = nvn_dirty_now(); nvn_dirty_protect(p, pg);
    dm_guest_write_prepare(p, 1); p[0] = 0x63;   // a host write: the pre-write hook unprotects it, no fault
    EXPECT("host write dirty", 1, nvn_dirty_since(p, pg, stamp));
    return NULL;
}
static void case_init(void) {
    EXPECT("fault hook registered before any pool", 1, fault_hook != NULL);
    EXPECT("pre-write hook registered before any pool", 1, write_hook != NULL);
    EXPECT("page size known before any pool", (long long)pg, (long long)g_pg);
    uint8_t *spare = page_map(pg); for (int i = 0; i < N_INIT; i++) init_pages[i] = page_map(pg);
    pthread_t w, t[N_INIT];
    pthread_create(&w, NULL, pre_writer, spare);
    for (int i = 0; i < N_INIT; i++) pthread_create(&t[i], NULL, first_pool, (void *)(intptr_t)i);
    for (int i = 0; i < N_INIT; i++) pthread_join(t[i], NULL);
    atomic_store(&stop, 1); pthread_join(w, NULL);
    for (int i = 0; i < N_INIT; i++) nvn_dirty_remove_range(init_pages[i]);
}

// b65b9b0f: a pool destroyed and another registered over the same memory (the same slot). The new pool's data was written while the memory was in no pool, so no fault and
// no generation saw it; a stamp from the old pool's time must count as written, also through the handle kept from the old pool
static void case_born(void) {
    uint8_t *m = page_map(pg);
    nvn_dirty_add_range(m, sub); nvn_dirty_protect(m, sub);
    uint64_t stamp = nvn_dirty_now(); const void *kept = nvn_dirty_span(m, sub);
    EXPECT("a protected pool not written: clean", 0, nvn_dirty_since_span(kept, m, sub, stamp));
    nvn_dirty_remove_range(m);
    ((volatile uint8_t *)m)[0] = 3;   // the next pool's data, written with no pool registered: no fault
    nvn_dirty_add_range(m, sub);
    EXPECT("same memory, a new pool: an older stamp is dirty", 1, nvn_dirty_since(m, sub, stamp));
    EXPECT("the same, through the handle kept from the old pool", 1, nvn_dirty_since_span(kept, m, sub, stamp));
    uint64_t stamp2 = nvn_dirty_now();
    EXPECT("a stamp taken after the new pool's registration: clean", 0, nvn_dirty_since(m, sub, stamp2));
    nvn_dirty_remove_range(m);
}

// a3d58526: a page the tracker unprotected (NONE, a generation) that another subsystem made read-only. A store there must be handed on (not re-executed forever): here
// after REEXEC_MAX re-executions, and at once from then on. Once the tracker has made the page writable for a write again (a stamp), a fault that raced it is re-executed
static void case_foreign_ro(void) {
    uint8_t *m = page_map(pg);
    nvn_dirty_add_range(m, pg); nvn_dirty_protect(m, pg);
    m[0] = 1;   // faults and leaves the page NONE with a generation
    mprotect(m, pg, PROT_READ);
    int n = 0; while (n <= REEXEC_MAX && on_fault(m, 1, NULL)) n++;
    EXPECT("write fault on a NONE page another subsystem made read-only: re-executed REEXEC_MAX times, then handed on", REEXEC_MAX, n);
    EXPECT("... and handed on at once from then on", 0, on_fault(m, 1, NULL));
    mprotect(m, pg, PROT_READ | PROT_WRITE);
    nvn_dirty_protect(m, pg); dm_guest_write_prepare(m, 1);   // protected again, then a host write lets it through: NONE, stamped
    EXPECT("a write fault that raced the tracker's release: re-executed", 1, on_fault(m, 1, NULL));
    nvn_dirty_remove_range(m); munmap(m, pg);
}

// af616f54: a retirement staged between nvn_dirty_protect's lookup and its first page-state exchange. Afterwards the retired pool is untracked and really writable
static uint8_t *rpage; static _Atomic int removing, removed; static pthread_t retirer;
static void *retire(void *a) { (void)a; atomic_store(&removing, 1); nvn_dirty_remove_range(rpage); atomic_store(&removed, 1); return NULL; }
static void retire_stage(void) {
    stage_at_cas = 0; pthread_create(&retirer, NULL, retire, NULL);
    while (!atomic_load(&removing)) usleep(100);
    usleep(100000);   // the retirement finishes in this window unless protect holds it off
}
static void case_retire_protect(void) {
    rpage = page_map(pg); nvn_dirty_add_range(rpage, pg);
    stage_at_cas = 1; nvn_dirty_protect(rpage, pg); pthread_join(retirer, NULL);
    int fd = open("/dev/zero", O_RDONLY); ssize_t got = read(fd, rpage, 16); int err = got < 0 ? errno : 0; close(fd);
    EXPECT("the retirement finished", 1, atomic_load(&removed));
    EXPECT("the retired pool is untracked", 0, nvn_dirty_tracked(rpage, pg));
    if (got != 16) fprintf(stderr, "read() into the retired pool: errno %d\n", err);
    EXPECT("a host write into the retired pool", 16, got);
    mprotect(rpage, pg, PROT_READ | PROT_WRITE);
}

// 1df3182f: one thread registers, protects and retires pools of changing sizes over an arena (a slot's arrays reused and grown) while others store into it (the fault
// hook), prepare host writes into it (the pre-write hook) and ask whether it was written through kept handles. Every write lands without a crash, and every pool, once
// retired, leaves the arena writable
enum { PAGES = 64, LIVE = 4 };
static uint8_t *arena; static _Atomic uint64_t writes, preps, checks;
static uint32_t rnd(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }
static void *churn(void *arg) {
    int rounds = (int)(intptr_t)arg; uint32_t s = 0x9e3779b9; uint8_t *live[LIVE] = {0};
    for (int k = 0; k < rounds; k++) {
        int j = k % LIVE;
        if (live[j]) nvn_dirty_remove_range(live[j]);
        // A pool sub aligned, so neighbours share host pages: from 1 page - 3 sub (a quarter page) to PAGES / 2 pages long, starting at most PAGES / 2 - 1 pages + 3 sub
        // in, so it ends inside the arena (below PAGES pages)
        size_t first = rnd(&s) % (PAGES / 2), len = (1 + rnd(&s) % (PAGES / 2)) * pg - (rnd(&s) % 4) * sub;
        live[j] = arena + first * pg + (rnd(&s) % 4) * sub;
        if (!len || live[j] + len > arena + PAGES * pg) { fprintf(stderr, "reuse: pool %zu +%zu outside the arena\n", (size_t)(live[j] - arena), len); _exit(3); }
        nvn_dirty_add_range(live[j], len); nvn_dirty_protect(live[j], len);
    }
    for (int j = 0; j < LIVE; j++) if (live[j]) nvn_dirty_remove_range(live[j]);
    atomic_store(&stop, 1);
    return NULL;
}
static void *writer(void *arg) { uint32_t s = (uint32_t)(intptr_t)arg; while (!atomic_load(&stop)) { ((volatile uint8_t *)arena)[rnd(&s) % (PAGES * pg)] = (uint8_t)s; atomic_fetch_add(&writes, 1); } return NULL; }
static void *preparer(void *arg) {
    uint32_t s = (uint32_t)(intptr_t)arg;
    while (!atomic_load(&stop)) {
        size_t o = rnd(&s) % (PAGES * pg), len = 1 + rnd(&s) % (2 * pg); if (o + len > PAGES * pg) len = PAGES * pg - o;
        dm_guest_write_prepare(arena + o, len); memset(arena + o, (int)(s & 0xff), len); atomic_fetch_add(&preps, 1);
    }
    return NULL;
}
static void *checker(void *arg) {
    uint32_t s = (uint32_t)(intptr_t)arg; const void *span = NULL;
    while (!atomic_load(&stop)) {
        uint8_t *a = arena + rnd(&s) % (PAGES * pg - sub);
        if (!span || !(rnd(&s) & 15)) span = nvn_dirty_span(a, sub);   // kept, as packaged textures keep theirs, across retirements and reuses
        (void)nvn_dirty_since_span(span, a, sub, nvn_dirty_now() / 2); atomic_fetch_add(&checks, 1);
    }
    return NULL;
}
static void case_reuse(int rounds) {
    alarm(100);
    arena = page_map(PAGES * pg);
    pthread_t c, t[5];
    pthread_create(&t[0], NULL, writer, (void *)(intptr_t)0x1234567);
    pthread_create(&t[1], NULL, preparer, (void *)(intptr_t)0x2345678);
    pthread_create(&t[2], NULL, preparer, (void *)(intptr_t)0x3456789);
    pthread_create(&t[3], NULL, checker, (void *)(intptr_t)0x456789a);
    pthread_create(&t[4], NULL, checker, (void *)(intptr_t)0x56789ab);
    pthread_create(&c, NULL, churn, (void *)(intptr_t)rounds);
    pthread_join(c, NULL);
    for (int i = 0; i < 5; i++) pthread_join(t[i], NULL);
    int live, peak, cap; nvn_dirty_stat(&live, &peak, &cap);
    EXPECT("pools left registered", 0, live);
    for (size_t o = 0; o < PAGES * pg; o += pg) arena[o] = 1;   // every page writable again (a protected one faults with no range: 99)
    printf("reuse: %d rounds, %llu writes, %llu host writes, %llu checks\n", rounds, (unsigned long long)writes, (unsigned long long)preps, (unsigned long long)checks);
}

// a2c731e3: adjacent pools share a host page after rounding, and only the range a texture was protected in gets the write's generation; looking up a different range
// that holds the span (nxnative's last-range shortcut) missed the write. Lookups stay first-match. Pages as host pages p0..p3:
//   starts_later 0: first [p0, p1 - sub) -> range p0..p1; second [p1 - sub, p3 - sub) -> range p0..p3; they share p0
//   starts_later 1: first [p2 - sub, p2) -> range p1..p2; second [p0, p2 - sub) -> range p0..p2; they share p1
static void case_overlap(int starts_later) {
    uint8_t *m = page_map(4 * pg), *first, *second; const void *other_span; size_t nfirst, nsecond;
    if (starts_later) { first = m + 2 * pg - sub; nfirst = sub; second = m; nsecond = 2 * pg - sub; other_span = m; }
    else { first = m; nfirst = pg - sub; second = m + pg - sub; nsecond = 2 * pg; other_span = m + pg; }
    nvn_dirty_add_range(first, nfirst);
    const void *first_range = nvn_dirty_span(first, 16);
    nvn_dirty_add_range(second, nsecond);
    uint64_t stamp = nvn_dirty_now();
    nvn_dirty_protect(first, 16);
    const void *other = nvn_dirty_span(other_span, pg);
    first[0] = 7;
    EXPECT("another range looked up in between", 1, first_range != other);
    EXPECT("the write is seen", 1, nvn_dirty_since(first, 16, stamp));
    EXPECT("the first range holding the span is found", 1, nvn_dirty_span(first, 16) == first_range);
    nvn_dirty_remove_range(first); nvn_dirty_remove_range(second); munmap(m, 4 * pg);
}

int main(int argc, char **argv) {
    pg = (size_t)getpagesize(); sub = pg / 4;
    struct sigaction sa = {0}; sa.sa_sigaction = handle_fault; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    const char *c = argc > 1 ? argv[1] : "";
    if (!strcmp(c, "init")) case_init();
    else if (!strcmp(c, "born")) case_born();
    else if (!strcmp(c, "foreign_ro")) case_foreign_ro();
    else if (!strcmp(c, "retire_protect")) case_retire_protect();
    else if (!strcmp(c, "reuse")) case_reuse(argc > 2 ? atoi(argv[2]) : 20000);
    else if (!strcmp(c, "overlap")) { case_overlap(0); case_overlap(1); }
    else if (!strcmp(c, "pagesize")) printf("%zu\n", pg);
    else { fprintf(stderr, "no case %s\n", c); return 2; }
    return g_fail != 0;
}
'''


def build(tmp, dirty_src, arch=None):
    dirty = '\n'.join(s for s in dirty_src.splitlines() if not s.startswith('#include'))
    c, exe = Path(tmp) / 'races.c', Path(tmp) / (f'races-{arch}' if arch else 'races')
    c.write_text(HARNESS.replace('DIRTY', dirty), encoding='utf-8')
    # -Wno-misleading-indentation: gcc refuses nvn_dirty.c's one-line release_page under -Werror (as test_dirty_swap)
    subprocess.run(['cc', *(['-arch', arch] if arch else []), '-std=gnu11', '-pthread', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-parameter',
                    '-Wno-misleading-indentation', str(c), '-o', str(exe)], check=True, capture_output=bool(arch))
    return exe


def build_4k(tmp, dirty_src):
    """The harness built for x86_64 on an arm64 Mac and run under Rosetta, whose processes have 4 KB pages (16 KB native), as Android and the Linux runners have:
    the cases run at both page sizes on the Mac. None when it does not build or run there, or the pages are not 4 KB"""
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        return None
    try:
        exe = build(tmp, dirty_src, 'x86_64')
        r = subprocess.run([str(exe), 'pagesize'], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    return exe if r.returncode == 0 and r.stdout.strip() == '4096' else None


@unittest.skipUnless(host_cc.posix(), 'a POSIX host C compiler is required (<sys/mman.h>, sigaction)')
class DirtyRaces(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='nx2a-dirty-races-')
        cls.exe = build(cls.tmp.name, (DM / 'gfx/nvn/nvn_dirty.c').read_text(encoding='utf-8'))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, *args, timeout=60):
        r = subprocess.run([str(self.exe), *args], capture_output=True, text=True, timeout=timeout)
        self.assertEqual(r.returncode, 0, f'{args[0]}: exit {r.returncode}\n{r.stdout}{r.stderr}')

    def test_hooks_set_at_load_and_first_pools_at_once(self):
        self.run_case('init')

    def test_older_stamp_counts_as_written_after_reuse(self):
        self.run_case('born')

    def test_foreign_read_only_page_is_handed_on(self):
        self.run_case('foreign_ro')

    def test_protect_holds_off_a_retirement(self):
        self.run_case('retire_protect')

    def test_slot_reuse_under_readers(self):
        self.run_case('reuse', '20000', timeout=120)

    def test_overlapping_pools_first_match(self):
        self.run_case('overlap')


class DirtyRaces4K(DirtyRaces):
    """The same cases with 4 KB host pages on an arm64 Mac (build_4k). Linux and Android hosts mostly have 4 KB pages already and run DirtyRaces at that size"""
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='nx2a-dirty-races-4k-')
        cls.exe = build_4k(cls.tmp.name, (DM / 'gfx/nvn/nvn_dirty.c').read_text(encoding='utf-8'))
        if not cls.exe:
            cls.tmp.cleanup()
            raise unittest.SkipTest('4 KB pages: only on an arm64 Mac with Rosetta (x86_64 build)')


if __name__ == '__main__':
    unittest.main()
