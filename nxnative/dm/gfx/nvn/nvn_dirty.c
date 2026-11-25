// Write-protect dirty-page tracking for guest memory: pool pages of packaged textures (CPU writes, GPU reads) are set read-only after upload, and as soon as the game writes it faults -> mark dirty, unprotect;
// before use, check the write generation of the covered pages and re-upload if newer than at upload time. NVN semantics: a texture descriptor points to pool memory, and the GPU reads the current content every time
// (measured: the game writes streamed assets into texture storage that is still being sampled, without re-registering).
// Signal handler constraint: the page-fault path uses only atomic operations and mprotect, no allocation, no locks.
#include "nvn_dirty.h"
#include "../../host/dm.h"
#include <stdatomic.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <sched.h>
#include <fcntl.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

// Order of page state and mprotect: the side changing protection first sets the state to BUSY and sets the final state only after mprotect has landed; other threads wait when they see BUSY.
//   set read-only: NONE -> BUSY -> mprotect(read-only) -> PROT; unprotect: PROT -> BUSY -> mprotect(writable) -> NONE.
// Invariant: a page whose state is NONE is always writable, one whose state is PROT is always read-only. Setting read-only used to be "record PROT, then mprotect"; in between, another thread's unprotect-before-write
// (the movie decoder thread writing a frame into texture storage, a disk read into a guest buffer) could set the state back to NONE, then the mprotect landed => the page read-only with state NONE: a game thread writing it got only
// "re-execute" from the fault handler and spun forever; a host system call reading into it saw NONE in prep_write, skipped, and failed with EFAULT (reproduced by t_dirty_race).
// BUSY lasts at most one mprotect (plus a few atomic writes when setting read-only). The wait is capped at BUSY_WAIT_MS and reports red beyond it: in the fault handler it then returns "not our page" so the crash handler reports truthfully
enum { PG_NONE = 0, PG_PROT = 1, PG_BUSY = 2 };
enum { BUSY_WAIT_MS = 2000 };
// Tracked ranges = the memory pools (registered by MemoryPoolInitialize). After a pool is destroyed its live=0 slot is reused; the signal handler scans without locks.
// The table grows in blocks of 1,024 slots, up to 262,144; a block is never moved or freed, so the signal handler reads it without locks. A pool past the end goes untracked: its textures count
// as dirty and are re-uploaded at every use. The table was a fixed 256 slots (Zelda has about 300 pools at the title), then a fixed 4,096, and Breath of the Wild reaches 4,006 pools in its first
// shrine. The scans only cover slots in use, so the capacity itself costs nothing
enum { BLK_SHIFT = 10, BLK = 1 << BLK_SHIFT, NBLK = 256, MAX_RANGE = BLK * NBLK };
// Ranges are rounded to host pages (16 KB) but guest memory pools are only 4 KB aligned: adjacent pools share their first / last pages, so one address can fall in several registered ranges.
// So: "is this texture tracked" looks for a range that holds its whole span; a fault / host pre-write unprotects and stamps every range covering that page; pool destruction identifies its range by the pool's exact start mem.
// (Previously the first range containing the address was always taken: two SMBW ASTC 6x5 textures whose pool's first page is shared with a small preceding pool were judged untracked and re-uploaded on every use, about 6 MB per frame)
// span: per page, 1 + the first page of the nvn_dirty_protect call that protected it (one texture's storage); 0 when not protected by one
// last: the highest generation of any page of the range (a check of a range nothing was written in since its stamp scans no page)
// nmp: this module's mprotect calls on the range since it was registered or defragmented (nvn_dirty_defrag)
// prep: when the host last made pages of the range writable for a write of its own (prep_write; mach time)
// id: the registration's number (g_adds_total), set and read under g_slot_mu: nvn_dirty_swap lets the mutex go between slices and tells its range from a pool that took the slot since
// born: the clock when the range was registered. A page's generation starts at 0 then, so a stamp taken before that (content checked in a pool this memory held before, or while
// it was in no pool) could not see a write made in between: the old pool's pages were made writable when it was destroyed, and the game wrote the next pool's data into them
// without a fault. A stamp older than born therefore counts as written (nvn_dirty_since_span; seedds/nxnative b65b9b0f: Skyward Sword HD drew Skyloft with the opening's meshes)
// pins: the lock-free readers (the fault hook, the host pre-write hook, the dirty checks, nvn_dirty_protect, nvn_dirty_defrag, nvn_dirty_swap between its locked steps,
// nvn_dirty_runs) pin a slot before they read its fields past base and end, and add_range rewrites a retired slot for the next pool only once none holds it (pin). base and
// end are read before the pin, to pass over slots that cannot hold the address, so they are atomic
typedef struct { _Atomic uintptr_t base, end; uintptr_t mem; _Atomic uint8_t *state; _Atomic uint64_t *gen; _Atomic uint32_t *span; size_t cap; _Atomic int live, pins; _Atomic uint32_t nmp; _Atomic uint64_t prep; _Atomic uint64_t last; uint64_t id; _Atomic uint64_t born; } range_t;
static inline uintptr_t rb(range_t *r) { return atomic_load_explicit(&r->base, memory_order_relaxed); }
static inline uintptr_t re(range_t *r) { return atomic_load_explicit(&r->end, memory_order_relaxed); }
static _Atomic(range_t *) g_blk[NBLK];
static inline range_t *rg(int i) { return &atomic_load_explicit(&g_blk[i >> BLK_SHIFT], memory_order_acquire)[i & (BLK - 1)]; }
static _Atomic int g_nr;
static uintptr_t g_pg, g_shift;
static _Atomic uint64_t g_now = 1;
// A page written (made writable for a write): its new generation, then the range's highest (a compare-and-swap maximum: signal-safe, and a slower thread never lowers it)
static inline void stamp_page(range_t *r, uintptr_t p) {
    uint64_t g = atomic_fetch_add(&g_now, 1) + 1; atomic_store(&r->gen[p], g);
    uint64_t o = atomic_load(&r->last); while (o < g && !atomic_compare_exchange_weak(&r->last, &o, g)) {}
}
static _Atomic uint64_t g_faults, g_preps, g_released;
static _Atomic uint64_t g_adds_total;
static _Atomic int g_live, g_live_peak;   // number of registered pools and peak (for the memory readout line)
void nvn_dirty_stat(int *live, int *peak, int *cap) { *live = atomic_load(&g_live); *peak = atomic_load(&g_live_peak); *cap = MAX_RANGE; }
static _Atomic int g_overflow_logged;
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;   // Pool creation, retirement and protection serialize descriptor lifetime; the page-fault path does not touch this mutex
static _Atomic uint64_t g_mp_calls, g_mp_pages, g_mp_ticks;   // This module's mprotect: call count, pages covered, time spent (mach timebase, safe in the page-fault path too)
static _Atomic uint64_t g_mp_begun, g_mp_done;   // this module's protection changes begun and finished: on_fault tells from them whether its scan saw the protection as it is
static _Atomic int g_mp_red;
static int mp(uintptr_t a, size_t len, int prot) {
    atomic_fetch_add(&g_mp_begun, 1);
    uint64_t t = mach_absolute_time(); int rc = mprotect((void *)a, len, prot); atomic_fetch_add(&g_mp_done, 1);
    atomic_fetch_add(&g_mp_ticks, mach_absolute_time() - t); atomic_fetch_add(&g_mp_calls, 1); atomic_fetch_add(&g_mp_pages, len >> g_shift);
    if (rc && !atomic_exchange(&g_mp_red, 1)) LOG("[RED] dirty-page tracking: mprotect %p +%#zx failed (the process's mapping limit?)", (void *)a, len);
    return rc;
}

static mach_timebase_info_data_t g_tb;   // obtained when the host loads (dirty_init; the fault path only multiplies and divides)
static _Atomic int g_busy_red;
// Wait for this page to leave BUSY. Returns 1 = it has left; 0 = waited past the limit (reported red once). Usable in a signal handler: only atomic reads, mach_absolute_time and sched_yield
static int busy_wait(range_t *r, uintptr_t p) {
    uint64_t t0 = 0;
    for (unsigned n = 1; atomic_load(&r->state[p]) == PG_BUSY; n++) {
        if (n & 0x3ff) continue;
        uint64_t now = mach_absolute_time(); if (!t0) t0 = now;
        if ((now - t0) * g_tb.numer / g_tb.denom > (uint64_t)BUSY_WAIT_MS * 1000000ull) {
            if (!atomic_exchange(&g_busy_red, 1)) LOG("[RED] dirty-page tracking: page %p stuck 'busy' for over %d ms (the thread changing its protection has not finished), no longer waiting", (void *)(rb(r) + (p << g_shift)), BUSY_WAIT_MS);
            return 0; }
        sched_yield();
    }
    return 1;
}
static int live_at(int i) { return atomic_load_explicit(&rg(i)->live, memory_order_acquire); }
// The registered range that holds all of [a, a+len) (used for a texture's tracking / protection / dirty check)
static range_t *find_span(uintptr_t a, size_t len) {
    int n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) { range_t *r = rg(i); if (live_at(i) && a >= rb(r) && a + len <= re(r)) return r; }
    return NULL;
}
// Pin a slot found live, so that it is not registered again for another pool while this reader uses its fields and arrays. A reader that found it live, then had it retired and
// registered again over other memory (add_range picks the first free slot, so the one just retired), read the new pool's end with the old start: the page index went past the
// arrays (the fault hook wrote there), or a dirty check read the old born with the new zeroed generations and called a written page clean (seedds/nxnative 1df3182f).
// Both sides are seq_cst: the pin is counted before live is read, and nvn_dirty_remove_range clears live before add_range reads the count, so either add_range sees the pin and
// waits, or this reader sees live clear (or set again, with the new pool's fields complete). Returns 0, unpinned, when the slot is not live. Usable in the signal handler
static int pin(range_t *r) {
    atomic_fetch_add(&r->pins, 1);
    if (atomic_load(&r->live)) return 1;
    atomic_fetch_sub_explicit(&r->pins, 1, memory_order_release); return 0;
}
static void unpin(range_t *r) { atomic_fetch_sub_explicit(&r->pins, 1, memory_order_release); }
// r pinned when it is live and holds all of [a, a+len); NULL otherwise
static range_t *pin_holding(range_t *r, uintptr_t a, size_t len) {
    if (!r || !pin(r)) return NULL;
    if (a >= rb(r) && a + len <= re(r)) return r;
    unpin(r); return NULL;
}
// find_span for a reader outside g_slot_mu: the range pinned (the caller unpins it), trying the caller's kept range first. NULL = untracked, or retired at every try
static range_t *pin_span(range_t *kept, uintptr_t a, size_t len) {
    range_t *r = pin_holding(kept, a, len);
    for (int tries = 0; !r && tries < 4; tries++) { range_t *f = find_span(a, len); if (!f) return NULL; r = pin_holding(f, a, len); }
    return r;
}
static int unprotect_page(range_t *r, uintptr_t p) {   // The caller has already changed the state from PROT to BUSY. 0 = unprotected; else it stays protected
    atomic_fetch_add(&r->nmp, 1);
    if (mp(rb(r) + (p << g_shift), g_pg, PROT_READ | PROT_WRITE)) { atomic_store(&r->state[p], PG_PROT); return -1; }
    stamp_page(r, p);
    atomic_store(&r->state[p], PG_NONE);
    return 0;
}
// Unprotect one page of a range: if protected, unprotect and stamp it; if another thread is changing its protection, wait for it to finish and look again -- if the other side was setting read-only, the page is PROT after the wait and still has to be unprotected
// (an unprotect-before-write never reaches the fault handler, so it must be done here). Returns 1 = this page was protected by this module and is writable now; 2 = it was already
// NONE and this module made it writable once (a generation); 0 = it was never this module's, or the wait on BUSY timed out
static int release_page(range_t *r, uintptr_t p, _Atomic uint64_t *count) {
    for (;;) {
        uint8_t st = PG_PROT;
        if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { if (unprotect_page(r, p)) return 0;   // a write let through is the only way on: not ours, the crash handler reports it
            if (count) atomic_fetch_add(count, 1); return 1; }
        // already NONE: most often another thread just unprotected it (the fault happened before the unprotect), but a read-only protection another subsystem put on a page
        // this module made writable is not ours either, and claiming it re-executed the store forever (seedds/nxnative a3d58526): on_fault decides (none_writable)
        if (st != PG_BUSY) return atomic_load(&r->gen[p]) != 0 ? 2 : 0;
        if (!busy_wait(r, p)) return 0;
    }
}
// The kernel's protection of the host page at a now: writable (and not executable). Made only for a fault no range claimed as protected.
// On Android (and Linux) from /proc/self/maps, read with open/read alone and parsed in place (no stdio, no allocation: this runs in the signal handler). The address is
// compared without its top byte (Android's tagged pointers; the maps list untagged addresses)
static int page_writable(uintptr_t a) {
#ifdef __APPLE__
    vm_address_t pg = a & ~(g_pg - 1), va = pg; vm_size_t vs = 0; vm_region_basic_info_data_64_t bi; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj = MACH_PORT_NULL;
    if (vm_region_64(mach_task_self(), &va, &vs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &cnt, &obj) != KERN_SUCCESS || va > pg) return 0;
    return (bi.protection & (VM_PROT_WRITE | VM_PROT_EXECUTE)) == VM_PROT_WRITE;
#else
    a &= 0x00ffffffffffffffull;
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC); if (fd < 0) return 0;
    char buf[512], perm[3] = {0}; uintptr_t lo = 0, hi = 0; int field = 0, np = 0, found = -1;   // field: 0 start, 1 end, 2 permissions, 3 the rest of the line
    for (ssize_t n; found < 0 && (n = read(fd, buf, sizeof buf)) > 0; )
        for (ssize_t i = 0; i < n && found < 0; i++) {
            char c = buf[i]; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (c == '\n') { field = 0; lo = hi = 0; np = 0; continue; }
            if (field == 0) { if (d >= 0) lo = lo << 4 | (uintptr_t)d; else if (c == '-') field = 1; }
            else if (field == 1) { if (d >= 0) hi = hi << 4 | (uintptr_t)d; else if (c == ' ') field = 2; }
            else if (field == 2) {
                perm[np++] = c; if (np < 3) continue;
                field = 3;
                if (a < lo) found = 0;   // the lines are in address order: past it, unmapped
                else if (a < hi) found = perm[1] == 'w' && perm[2] != 'x';
            }
        }
    close(fd);
    return found > 0;
#endif
}
// A page this module made writable (NONE, with a generation) that faulted: re-executed at once, unless the last such re-execution of a page that hashes alike was of the same
// page; then the kernel is asked (page_writable). A read-only protection another subsystem put on it so faults at most twice before it is handed on. Asking the kernel every
// time, as seedds/nxnative does on the Mac (one vm_region call), would read all of /proc/self/maps here at every fault two threads race on one protected page (tens of
// thousands of lines in Xenoblade Chronicles 3)
static _Atomic uintptr_t g_none_last[64];
static int none_writable(uintptr_t a) {
    uintptr_t pg = a >> g_shift;
    return atomic_exchange(&g_none_last[pg & 63], pg) != pg || page_writable(a);
}
static void release_pages(range_t *r, uintptr_t p, uintptr_t q, _Atomic uint64_t *count);
// One pinned range over the faulting address a: its page released (1 = it was protected, so ours; *none set when it was NONE with a generation), and the rest of its texture with it
static int on_fault_range(range_t *r, uintptr_t a, int *none) {
    uintptr_t pg = (a - rb(r)) >> g_shift; uint32_t sp = atomic_load(&r->span[pg]);
    int rc = release_page(r, pg, &g_faults); if (rc == 2) *none = 1;
    // The rest of the storage the same protect call covered goes with it: the texture is uploaded again after any write into it, so this costs no upload, and the
    // game's writes into it fault once instead of once a page. Released a page at a time, a texture written at every other page left its storage in alternating
    // read-only and writable pages, each a mapping of its own: Xenoblade Chronicles 3 reached the kernel's mapping limit (65,530) in its first battle, after which
    // mprotect failed, a faulting write was never let through, and the game hung (or an UnmapPhysicalMemory failed with ENOMEM)
    if (sp) { uintptr_t q = sp - 1; while (q < r->cap && q < ((re(r) - rb(r)) >> g_shift) && atomic_load(&r->span[q]) == sp) q++; if (q > sp - 1) release_pages(r, sp - 1, q - 1, &g_faults); }
    return rc == 1;
}
// A write that faulted on a protected page may be handled only after a pool over it was retired: nvn_dirty_remove_range makes the pages writable, then drops the range, and the
// scan then found no range to claim the fault, so the crash handler stopped the host on a write that was the tracker's (SIGBUS; seedds/nxnative 1df3182f, t_dirty_reuse). So a
// fault no range claims is re-executed when the page is writable by now; and when it is not, but a protection change of this module ran during the scan (a pool registered over
// the page and protected it, say), the scan runs again. Only a scan and look with no change of this module during them hands the fault on as not ours (the next hook, or the crash path)
static int on_fault(void *addr, int is_write, void *uc) {
    (void)uc;
    if (!is_write) return 0;
    uintptr_t a = (uintptr_t)addr;
    for (int tries = 0; tries < 1000; tries++) {
        uint64_t done = atomic_load(&g_mp_done); int ours = 0, none = 0, n = atomic_load(&g_nr);
        for (int i = 0; i < n; i++) {   // Every range sharing the page must be released: releasing only one leaves the other range's state at "protected", and later writes no longer fault and are missed
            range_t *r = rg(i); if (!live_at(i) || a < rb(r) || a >= re(r) || !pin(r)) continue;
            if (a < rb(r) || a >= re(r)) { unpin(r); continue; }   // registered again over other memory between the look and the pin
            ours |= on_fault_range(r, a, &none);
            unpin(r);
        }
        if (ours || (none ? none_writable(a) : page_writable(a))) return 1;
        if (atomic_load(&g_mp_begun) == done) return 0;   // none begun since the scan began, none in flight then: the page's protection is not this module's
    }
    return 0;
}
// Unprotect pages p..q of a range with one mprotect per run of protected pages (a host write over a whole texture, e.g. WriteTexels, used to make one system call per 16 KB page:
// Link's Awakening, about 275 a frame). Pages another thread is changing are handled one by one after the run so far is finished, so no BUSY page is held while waiting
static void release_pages(range_t *r, uintptr_t p, uintptr_t q, _Atomic uint64_t *count) {
    uintptr_t run = 0; int open = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_PROT; int took = i <= q && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);
        if (took) { if (!open) { run = i; open = 1; } continue; }
        if (open) {
            atomic_fetch_add(&r->nmp, 1);
            if (mp(rb(r) + (run << g_shift), (i - run) << g_shift, PROT_READ | PROT_WRITE)) {
                // A failed mprotect may change only part of the run; neither writable states nor a retry are safe. Also called from the fault handler.
                static const char msg[] = "[dm] [RED] dirty-page tracking: cannot make guest memory writable; stopping\n";
                ssize_t w = write(STDERR_FILENO, msg, sizeof msg - 1); (void)w; _exit(1);   // (glibc's write is warn_unused_result, which a (void) cast does not quiet in gcc)
            }
            for (uintptr_t k = run; k < i; k++) { stamp_page(r, k); atomic_store(&r->state[k], PG_NONE); }
            if (count) atomic_fetch_add(count, i - run);
            open = 0;
        }
        if (i <= q && st == PG_BUSY) release_page(r, i, count);   // another thread is changing it: wait, then unprotect it if it ended up protected
    }
}
static void release_range(const void *addr, size_t len, _Atomic uint64_t *count) {
    uintptr_t a = (uintptr_t)addr, e = a + len; int n = atomic_load(&g_nr);
    if (!len) return;
    for (int i = 0; i < n; i++) {
        range_t *r = rg(i);
        if (!live_at(i) || e <= rb(r) || a >= re(r) || !pin(r)) continue;
        uintptr_t base = rb(r), end = re(r);   // pinned: these hold until the unpin
        if (e > base && a < end) {
            uintptr_t lo = a > base ? a : base, hi = e < end ? e : end;
            if (count == &g_preps) atomic_store(&r->prep, mach_absolute_time());
            release_pages(r, (lo - base) >> g_shift, (hi - 1 - base) >> g_shift, count);
        }
        unpin(r);
    }
}
static void prep_write(void *addr, size_t len) { release_range(addr, len, &g_preps); }
// A caller that no longer needs a span watched (a packaged texture not drawn for a while, gfx_vk_tex.c vk_tex_sweep) gives up its protection: the pages are stamped as written, so
// the texture is uploaded and protected again when it is drawn. Textures stay protected until the game writes them, so the protected runs, each splitting its pool's mapping,
// would grow with every place the player has seen
void nvn_dirty_release(const void *addr, size_t len) { if (len) release_range(addr, len, &g_released); }
uint64_t nvn_dirty_released(void) { return atomic_load(&g_released); }
// Defragmenting: the pools' pages are a dma-buf's (gfx_vk_core.c zero copy), and the kernel never merges such mappings again, so every protect or unprotect boundary
// ever made stayed a mapping of its own whatever the protection was now (Xenoblade Chronicles 3: 10,600 read-only mappings with 475 runs of pages protected, 43,000 in
// all after 25 minutes of walking, toward the kernel's limit of 65,530). A range with more than DEFRAG_MP mprotect calls since the last time is mapped again in one piece:
// every page is made BUSY (a game write faulting on one waits), remap() maps the whole range read-only in one call, the runs that were writable are made writable again
// (and stamped as written, so a write that waited is the module's to let through), and the protected pages keep their protection. The mappings are then about twice the
// runs. Run on the thread that protects textures (the replay's); remap() returns 0 when it mapped the range, 1 when it mapped nothing (no protection changed: the
// pages keep their states, as a range remap() cannot take, such as a pool not whole host pages, whose range is rounded up to them), -1 when it failed after mapping a
// part read-only: the runs that were writable are then made writable again as after a success. A run that cannot be made writable again (mprotect fails, likely
// for the same reason the remap did: the mapping limit) stays read-only and is kept as protected (its next write faults and is let through), as nvn_dirty_swap does,
// so a page whose state is NONE is never read-only.
// A range the host prepared a write into in the last DEFRAG_QUIET_MS is left for later: the host's own writes (a file read into a pool, which the game does for its
// streaming) are system calls the kernel fails with EFAULT on a page made read-only under them; Xenoblade Chronicles 3 then inflated a short read and crashed in zlib
enum { DEFRAG_MP = 4000, DEFRAG_QUIET_MS = 1000 };
static int written_lately(range_t *r) { return (mach_absolute_time() - atomic_load(&r->prep)) * g_tb.numer / g_tb.denom < (uint64_t)DEFRAG_QUIET_MS * 1000000ull; }
// One range, pinned by the caller: 1 = mapped again, 0 = not (left for later, or as it was)
static int defrag_one(range_t *r, int (*remap)(void *mem, size_t len)) {
    {
        if (atomic_load(&r->nmp) < DEFRAG_MP || r->mem != rb(r)) return 0;   // a pool starting mid-page shares that page: left alone
        if (written_lately(r)) return 0;
        size_t np = (re(r) - rb(r)) >> g_shift;
        uint8_t *was = malloc(np); if (!was) return 0;
        for (size_t p = 0; p < np; p++) {   // every page BUSY; was = its state before
            for (;;) { uint8_t st = atomic_load(&r->state[p]);
                if (st == PG_BUSY) { if (!busy_wait(r, p)) break; continue; }
                if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { was[p] = st; break; } }
        }
        int rm = remap((void *)rb(r), re(r) - rb(r)), ok = rm == 0;
        if (rm > 0) { for (size_t p = 0; p < np; p++) atomic_store(&r->state[p], was[p]); free(was); atomic_store(&r->nmp, 0); return 0; }   // nothing mapped: as it was
        for (size_t p = 0; p < np; ) {   // the pages that were writable, a run at a time (also after a remap that failed after it mapped a part read-only)
            if (was[p] == PG_PROT) { p++; continue; }
            size_t q = p; while (q < np && was[q] != PG_PROT) q++;
            if (mp(rb(r) + (p << g_shift), (q - p) << g_shift, PROT_READ | PROT_WRITE)) memset(was + p, PG_PROT, q - p);   // left read-only: kept as protected
            p = q;
        }
        for (size_t p = 0; p < np; p++) {
            if (was[p] == PG_PROT) { atomic_store(&r->state[p], PG_PROT); continue; }
            stamp_page(r, p); atomic_store(&r->state[p], PG_NONE);   // (a write that waited on a writable page is let through, after a failed remap too)
        }
        free(was);
        atomic_store(&r->nmp, 0); return ok;   // a range that could not be mapped again is tried after another DEFRAG_MP calls, not at every sweep
    }
}
// The range is pinned while its pages are BUSY and remapped: retired meanwhile, its slot is not given to another pool (whose arrays the restore would write) before the unpin
int nvn_dirty_defrag(int (*remap)(void *mem, size_t len)) {
    int done = 0, n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) {
        range_t *r = rg(i); if (!live_at(i) || atomic_load(&r->nmp) < DEFRAG_MP || !pin(r)) continue;
        done += defrag_one(r, remap); unpin(r);
    }
    return done;
}
// [mem, mem+len) of one range given new pages by fill (gfx_vk_core.c: a lazy pool's buffer, the first time the GPU uses it), SWAP_SLICE bytes at a time. As in nvn_dirty_defrag
// every page of a slice is made BUSY, and also read-only, so no write lands while fill copies its old pages into the new ones; fill(at, n, off, ctx) maps the new pages over
// [at, at+n), off bytes into the span, read-only (0 = done). Then the slice's runs that were writable are made writable again and stamped as written (a write that waited on a
// BUSY page is the module's to let through), and the protected pages keep their protection; a run that cannot be made writable again stays read-only as protected (its next
// write faults and is let through as any other), so a page whose state is NONE is never read-only. A slice at a time, so a page stays BUSY for one slice's copy, not the
// span's: every wait on a BUSY page gives up after BUSY_WAIT_MS, and a game write that gives up is reported as a crash.
// A range the host prepared a write into in the last DEFRAG_QUIET_MS is left for later, as in nvn_dirty_defrag: a system call that prepared its pages before they went BUSY
// may still be writing, and fails with EFAULT on a page made read-only under it. Checked with the slice's pages BUSY: a prep_write after that waits for them (release_pages).
// → 0; 2 when the host wrote into the range lately (try later); fill's error, or -1 (a page stayed BUSY past BUSY_WAIT_MS, mprotect failed, the span is not whole host pages);
// 1 when no registered range holds the span (nothing done), or the range was retired between two slices. done (may be NULL): the bytes from mem whose slices were swapped
// before it stopped, which keep the new pages (a caller that drops them maps those slices back first: gfx_vk_core.c zc_unplace, whose put-back of a retired range finds
// none and leaves them to mtl_pool_del's copy_back through vkpool_t left)
// The range is looked up, checked and each slice's pages made BUSY with g_slot_mu held, which pool registration and retirement hold, as nvn_dirty_protect does
// (seedds/nxnative af616f54). Looked up without it, a retirement (hle_nvn.c: nvn_dirty_remove_range, then mtl_pool_del, which waits on g_lazy_mu while pool_back swaps)
// could finish between the lookup and BUSY: its prep_write made the pages writable and live went 0, and the swap then made the retired pool's pages read-only with no live
// range to let a write through (a game write faulted with no owner, a host read() into them failed with EFAULT), or worked from another pool's base and state array once
// that pool took the slot (its prep reset, so no "written lately" stopped it); and the range of a second lookup after nvn_dirty_quiet's was used unchecked, NULL once a
// retirement fell between the two. The mutex is let go before mprotect and fill: a retirement during the
// copy waits in its prep_write on the BUSY pages, which are given back without the mutex, so nothing holds it while it waits on the swap. The waits on BUSY pages with it
// held are on the fault and host pre-write paths, which take no lock. Between slices the range is checked again (live, and the same registration: id)
// Ranges overlap: a pool made inside another (Kirby's pools over files in its heap pool, gfx_vk_core.c "Pools inside pools", #239) registers a range inside the outer
// one's, and pools that are not page-aligned share their edge pages. Each range keeps its own page states, so a slice is taken BUSY in every live range over it, as
// seedds/nxnative 1e69c180 takes every range's pages in nvn_dirty_open before it opens any. Taken in the swapped range alone, a page that another range had protected
// (a texture of the inner pool) was made writable again after fill, as the swapped range's state said, while that range still said PROT: the game's later writes to it
// never faulted and the texture stayed stale. And a write during the copy faulted, the other range let it through, so the page was writable, and another thread's write
// to that storage went into the old page after the copy and was lost when fill mapped the new one over it. Now a page stays read-only when any range over it had it
// protected, and each range gets its own state back (stamped where the page is writable again)
enum { SWAP_SLICE = 16 << 20 };
// nvn_dirty_swap's answer before it touches a page: 0 = a registered range holds the span and the host has prepared no write into it in the last DEFRAG_QUIET_MS (go
// ahead); 1 = no registered range holds it; 2 = the host wrote into it lately (try later); -1 = not whole host pages. gfx_vk_core.c asks before it takes a buffer for a
// lazy pool, so a pool the host keeps writing into costs no allocation each time it is tried again. With g_slot_mu held, so the prep read is the range's that was found
// (a retirement in between could hand the slot to another pool). Ranges inside ranges need nothing more here: a host write stamps prep in every live range it
// overlaps (release_range), so the range found, which holds the whole span, has seen every write into it
int nvn_dirty_quiet(const void *mem, size_t len) {
    if (!g_pg || (((uintptr_t)mem | len) & (g_pg - 1))) return -1;   // whole host pages only
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = find_span((uintptr_t)mem, len); int q = !r ? 1 : written_lately(r) ? 2 : 0;
    pthread_mutex_unlock(&g_slot_mu);
    return q;
}
// One range's pages p..p+n-1 taken for nvn_dirty_swap: each made BUSY, its state before into was[] (*k: the pages taken so far). -1 = one stayed BUSY past BUSY_WAIT_MS.
// The pages taken are held while it waits: no other path waits while it holds a BUSY page (the fault and host pre-write paths, nvn_dirty_protect) but
// nvn_dirty_defrag, which runs on the replay's thread, as the swap does (gfx_vk_core.c pool_back)
static int swap_take(range_t *r, uintptr_t p, size_t n, uint8_t *was, size_t *k) {
    while (*k < n) {
        uint8_t st = atomic_load(&r->state[p + *k]);
        if (st == PG_BUSY) { if (!busy_wait(r, p + *k)) return -1; continue; }
        if (atomic_compare_exchange_strong(&r->state[p + *k], &st, PG_BUSY)) was[(*k)++] = st;
    }
    return 0;
}
// A range over part of a slice: its pages p..p+n-1, the slice's pages j..j+n-1, k of them taken; their states before in was[], after the previous take's n
typedef struct { range_t *r; uintptr_t p; size_t n, j, k; } take_t;
int nvn_dirty_swap(void *mem, size_t len, int (*fill)(void *at, size_t n, size_t off, void *ctx), void *ctx, size_t *done) {
    if (done) *done = 0;
    int q = nvn_dirty_quiet(mem, len); if (q) return q;   // (written lately: checked again below with each slice BUSY; this spares the first slice's waits)
    range_t *r = NULL; uint64_t id = 0;
    size_t sp = (len < SWAP_SLICE ? len : SWAP_SLICE) >> g_shift, wcap = 2 * sp, tcap = 8; int rc = 0;
    uint8_t *ro = malloc(sp), *was = malloc(wcap); take_t *tk = malloc(tcap * sizeof *tk);   // ro: per page of the slice, 1 = a range over it protects it, 2 = left read-only
    if (!ro || !was || !tk) { free(ro); free(was); free(tk); return -1; }
    for (size_t off = 0; off < len && !rc; off += sp << g_shift) {
        size_t n = len - off < (sp << g_shift) ? len - off : sp << g_shift, np = n >> g_shift, nt = 0, nw = 0;
        uintptr_t a = (uintptr_t)mem + off, e = a + n;
        pthread_mutex_lock(&g_slot_mu);   // (see above: the lookup, the check and the BUSY pages under it, mprotect and fill after it)
        if (!r && (r = find_span((uintptr_t)mem, len))) id = r->id;
        if (!r || !atomic_load_explicit(&r->live, memory_order_acquire) || r->id != id) { pthread_mutex_unlock(&g_slot_mu); rc = 1; break; }   // retired since (or before the first slice)
        int nr = atomic_load(&g_nr);
        for (int i = 0; i < nr && !rc; i++) {   // the slice's pages BUSY in every live range over them (r among them, whole)
            range_t *o = rg(i); if (!live_at(i) || e <= rb(o) || a >= re(o)) continue;
            uintptr_t lo = a > rb(o) ? a : rb(o), hi = e < re(o) ? e : re(o); size_t m = (hi - lo) >> g_shift;
            if (nt == tcap) { take_t *t = realloc(tk, 2 * tcap * sizeof *tk); if (!t) { rc = -1; break; } tk = t; tcap *= 2; }
            if (nw + m > wcap) { size_t c = nw + m > 2 * wcap ? nw + m : 2 * wcap; uint8_t *w = realloc(was, c); if (!w) { rc = -1; break; } was = w; wcap = c; }
            pin(o);   // (live, and g_slot_mu held: no retirement in between) held until its states are back, past the unlock: its slot is not given to another pool meanwhile
            tk[nt] = (take_t){o, (lo - rb(o)) >> g_shift, m, (lo - a) >> g_shift, 0};
            rc = swap_take(o, tk[nt].p, m, was + nw, &tk[nt].k); nw += m; nt++;
        }
        if (!rc && written_lately(r)) rc = 2;   // (r holds the slice: a host write into any range over it stamped r's prep too)
        if (rc) {
            for (size_t t = 0, w = 0; t < nt; w += tk[t++].n) for (size_t j = 0; j < tk[t].k; j++) atomic_store(&tk[t].r->state[tk[t].p + j], was[w + j]);
            for (size_t t = 0; t < nt; t++) unpin(tk[t].r);
            pthread_mutex_unlock(&g_slot_mu); break;
        }
        pthread_mutex_unlock(&g_slot_mu);
        rc = mp(a, n, PROT_READ) ? -1 : fill((void *)a, n, off, ctx);   // (a failed mprotect may have made part of it read-only: the writable runs are made writable again below)
        memset(ro, 0, np);
        for (size_t t = 0, w = 0; t < nt; w += tk[t++].n) for (size_t j = 0; j < tk[t].n; j++) if (was[w + j] == PG_PROT) ro[tk[t].j + j] = 1;
        for (size_t j = 0; j < np; ) {   // the runs no range protects made writable again; one that cannot be stays read-only, protected in every range (2)
            if (ro[j]) { j++; continue; }
            size_t q = j; while (q < np && !ro[q]) q++;
            if (mp(a + (j << g_shift), (q - j) << g_shift, PROT_READ | PROT_WRITE)) memset(ro + j, 2, q - j);
            j = q;
        }
        for (size_t t = 0, w = 0; t < nt; w += tk[t++].n) for (size_t j = 0; j < tk[t].n; j++) {   // each range's own state back (NONE under another range's PROT stays NONE, as before)
            uint8_t st = was[w + j], pr = ro[tk[t].j + j];
            if (st == PG_NONE && !pr) stamp_page(tk[t].r, tk[t].p + j); else if (st == PG_NONE && pr == 2) st = PG_PROT;
            atomic_store(&tk[t].r->state[tk[t].p + j], st);
        }
        for (size_t t = 0; t < nt; t++) unpin(tk[t].r);
        if (!rc && done) *done = off + n;
    }
    free(ro); free(was); free(tk);
    return rc;
}
// Runs of protected pages over every live range: each is a mapping of its own (at most two with the writable pieces around it)
int nvn_dirty_runs(void) {
    int n = atomic_load(&g_nr), runs = 0;
    for (int i = 0; i < n; i++) {
        range_t *r = rg(i); if (!live_at(i) || !pin(r)) continue;
        size_t np = (re(r) - rb(r)) >> g_shift; int prev = 0;
        for (size_t p = 0; p < np; p++) { int pr = atomic_load_explicit(&r->state[p], memory_order_relaxed) != PG_NONE; runs += pr && !prev; prev = pr; }
        unpin(r);
    }
    return runs;
}

// Page geometry, the timebase and both hooks are set when the library loads, on one thread before any game thread runs: every pool registration, fault and host pre-write
// sees them complete. They were set by the first nvn_dirty_add_range behind a plain flag, published before the geometry and the hooks and with no lock: a pool registered on
// another thread at that moment could get zero geometry or return before the hooks existed, and a host write into guest memory (hle_fs.c reading a file on a game thread) read
// crash.c's hook table while it grew (seedds/nxnative de2018e0). With no pool registered the hooks scan an empty table, so a game that never makes one pays nothing for them
__attribute__((constructor)) static void dirty_init(void) {
    g_pg = (uintptr_t)getpagesize(); g_shift = __builtin_ctzl(g_pg); mach_timebase_info(&g_tb);
    dm_fault_hook_register(on_fault); dm_guest_write_prep_register(prep_write);
}

void nvn_dirty_add_range(const void *mem, size_t size) {
    uint64_t id = atomic_fetch_add(&g_adds_total, 1) + 1;
    pthread_mutex_lock(&g_slot_mu);
    int n = atomic_load(&g_nr);
    int slot = -1;
    for (int i = 0; i < n; i++) if (!live_at(i)) { slot = i; break; }
    if (slot < 0) {
        if (n >= MAX_RANGE) {
            int expected = 0;
            if (atomic_compare_exchange_strong(&g_overflow_logged, &expected, 1)) LOG("[RED] dirty-page tracking ranges are full (%d), pool %p not tracked", MAX_RANGE, mem);
            pthread_mutex_unlock(&g_slot_mu); return;
        }
        slot = n;
        if (!atomic_load_explicit(&g_blk[n >> BLK_SHIFT], memory_order_acquire)) {   // published before g_nr counts the slot (the scans read g_nr first)
            range_t *b = calloc(BLK, sizeof *b); if (!b) die("dirty-page tracking range table allocation failed (block %d)", n >> BLK_SHIFT);
            atomic_store_explicit(&g_blk[n >> BLK_SHIFT], b, memory_order_release);
        }
    }
    { int lv = atomic_fetch_add(&g_live, 1) + 1, pk = atomic_load(&g_live_peak); while (lv > pk && !atomic_compare_exchange_weak(&g_live_peak, &pk, lv)) {} }
    range_t *r = rg(slot);
    while (atomic_load(&r->pins)) sched_yield();   // a reader that found the retired pool live still uses its fields (pin); one in the fault hook may wait out a BUSY page (BUSY_WAIT_MS at most)
    uintptr_t base = (uintptr_t)mem & ~(g_pg - 1), end = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
    size_t np = (end - base) >> g_shift;
    _Atomic uint8_t *state = r->state;
    _Atomic uint64_t *gen = r->gen;
    if (r->cap < np) {
        free((void *)state); free((void *)gen); free((void *)r->span);   // no reader holds them: none is pinned, and a reader pins before it reads the pointers
        state = calloc(np, 1); gen = calloc(np, sizeof *gen); r->span = calloc(np, sizeof *r->span);
        if (!state || !gen || !r->span) die("dirty-page table allocation failed (%zu pages)", np);
        r->cap = np;
    } else {
        memset((void *)state, 0, np); memset((void *)gen, 0, np * sizeof *gen); memset((void *)r->span, 0, np * sizeof *r->span);
    }
    atomic_store_explicit(&r->base, base, memory_order_relaxed); atomic_store_explicit(&r->end, end, memory_order_relaxed); r->mem = (uintptr_t)mem; r->state = state; r->gen = gen;
    atomic_store(&r->nmp, 0); atomic_store(&r->prep, 0); atomic_store(&r->last, 0);
    r->id = id;
    atomic_store_explicit(&r->born, atomic_fetch_add(&g_now, 1) + 1, memory_order_relaxed); atomic_fetch_add(&g_now, 1);   // published by the release store of live below; stamps taken from now on are past it
    if (slot == n) atomic_store(&g_nr, n + 1);
    atomic_store_explicit(&r->live, 1, memory_order_release);
    pthread_mutex_unlock(&g_slot_mu);
}
// Pool destruction: first restore still-protected pages to writable (the pool memory may be reused for something else once returned to the game), then mark invalid
void nvn_dirty_remove_range(const void *mem) {
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = NULL; int n = atomic_load(&g_nr);
    for (int i = 0; i < n && !r; i++) if (live_at(i) && rg(i)->mem == (uintptr_t)mem) r = rg(i);   // Match by the pool's exact start: an adjacent pool sharing a page may have the same rounded base
    if (r) { prep_write((void *)rb(r), re(r) - rb(r)); atomic_store(&r->live, 0); atomic_fetch_sub(&g_live, 1); }   // seq_cst: against pin (the reuse in add_range reads the pin count)
    pthread_mutex_unlock(&g_slot_mu);
}
int nvn_dirty_tracked(const void *addr, size_t len) {
    return find_span((uintptr_t)addr, len) != NULL;
}
// The tracked range holding [addr, addr+len), as a handle a caller keeps (packaged textures: the scan over every range ran twice per texture lookup). NULL = untracked
const void *nvn_dirty_span(const void *addr, size_t len) { return len ? find_span((uintptr_t)addr, len) : NULL; }
// Set read-only: mark the pages BUSY first and mark them PROT only after mprotect has landed (for the order and the invariant see the state notes at the top of the file)
// g_slot_mu is held from the lookup through the last mprotect (seedds/nxnative af616f54): a retirement that finished between the lookup and the first page-state change
// had made the pages writable and the range dead, and protect then set the retired storage read-only with no live range to release it (a host read() into it failed with
// EFAULT, a game write faulted with no owner); in the same window a new pool could take the slot. The fault and host pre-write paths take no lock, and protect waits on none
void nvn_dirty_protect(const void *addr, size_t len) {
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = len ? find_span((uintptr_t)addr, len) : NULL;
    if (!r) { pthread_mutex_unlock(&g_slot_mu); return; }
    uintptr_t a = (uintptr_t)addr, p = (a - rb(r)) >> g_shift, q = (a + len - 1 - rb(r)) >> g_shift;
    // Consecutive pages are merged into one mprotect (per-page calls reach thousands per frame on the first upload of large textures; character select page startup frame 5: 4888 calls).
    // Pages already PROT / BUSY break the consecutive run: do not protect again, and do not touch pages another thread is changing
    uintptr_t run = 0; int open = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_NONE;
        int took = i <= q && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);
        if (took && !open) { run = i; open = 1; }
        if (!took && open) { atomic_fetch_add(&r->nmp, 1); mp(rb(r) + (run << g_shift), (i - run) << g_shift, PROT_READ); for (uintptr_t k = run; k < i; k++) { atomic_store(&r->span[k], (uint32_t)p + 1); atomic_store(&r->state[k], PG_PROT); } open = 0; }
    }
    pthread_mutex_unlock(&g_slot_mu);
}
uint64_t nvn_dirty_now(void) { return atomic_load(&g_now); }
// The kept handle is tried first: it still holds the span (a slot reused by another pool over the same memory holds it too, and a stamp older than that pool's registration
// then counts as written: born), so no scan. The range is pinned while its fields are read
int nvn_dirty_since_span(const void *span, const void *addr, size_t len, uint64_t stamp) {
    if (!len) return 0;
    uintptr_t a = (uintptr_t)addr; range_t *r = pin_span((range_t *)span, a, len);
    if (!r) return 1;   // When untracked, conservatively treat as dirty: better to upload more than to show stale content
    int w = stamp < atomic_load_explicit(&r->born, memory_order_relaxed);   // checked before this pool was registered over the memory: what was written there in between left no generation (born)
    if (!w && atomic_load(&r->last) > stamp) {   // (last: nothing written in the range since the stamp scans no page)
        uintptr_t p = (a - rb(r)) >> g_shift, q = (a + len - 1 - rb(r)) >> g_shift;
        for (uintptr_t i = p; i <= q && !w; i++) w = atomic_load(&r->gen[i]) > stamp;   // The generation of the last write before upload is <= the stamp; any later write must be > the stamp
    }
    unpin(r);
    return w;
}
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) { return nvn_dirty_since_span(NULL, addr, len, stamp); }
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total) {
    int n = atomic_load(&g_nr), active = 0;
    for (int i = 0; i < n; i++) active += live_at(i) != 0;
    *live = active; *used_slots = n; *adds_total = atomic_load(&g_adds_total);
}
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps) { *faults = atomic_load(&g_faults); *preps = atomic_load(&g_preps); }
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    *calls = atomic_load(&g_mp_calls); *pages = atomic_load(&g_mp_pages); *ns = atomic_load(&g_mp_ticks) * tb.numer / tb.denom;
}
