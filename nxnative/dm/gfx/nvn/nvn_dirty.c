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
// blk: one bit per 1 << blk_shift bytes from mem, set as a page of the block is made writable (nvn_dirty_blocks; NULL until a caller asks for it)
typedef struct { uintptr_t base, end, mem; _Atomic uint8_t *state; _Atomic uint64_t *gen; _Atomic uint32_t *span; size_t cap; _Atomic int live; _Atomic uint32_t nmp; _Atomic uint64_t prep; _Atomic uint64_t last;
                 _Atomic(_Atomic uint64_t *) blk; unsigned blk_shift; size_t nblk_words; } range_t;
static _Atomic(range_t *) g_blk[NBLK];
static inline range_t *rg(int i) { return &atomic_load_explicit(&g_blk[i >> BLK_SHIFT], memory_order_acquire)[i & (BLK - 1)]; }
static _Atomic int g_nr;
static uintptr_t g_pg, g_shift;
static _Atomic uint64_t g_now = 1;
// A page written (made writable for a write): its new generation, then the range's highest (a compare-and-swap maximum: signal-safe, and a slower thread never lowers it)
static inline void stamp_page(range_t *r, uintptr_t p) {
    uint64_t g = atomic_fetch_add(&g_now, 1) + 1; atomic_store(&r->gen[p], g);
    uint64_t o = atomic_load(&r->last); while (o < g && !atomic_compare_exchange_weak(&r->last, &o, g)) {}
    _Atomic uint64_t *b = atomic_load_explicit(&r->blk, memory_order_acquire);
    if (b) {   // (after the page's generation: a caller that takes the bit finds the generation too)
        uintptr_t a = r->base + (p << g_shift), lo = a > r->mem ? a - r->mem : 0, hi = a + g_pg - r->mem;
        for (uintptr_t k = lo >> r->blk_shift; k <= (hi - 1) >> r->blk_shift && k >> 6 < r->nblk_words; k++) atomic_fetch_or(&b[k >> 6], 1ull << (k & 63));
    }
}
static _Atomic uint64_t g_faults, g_preps, g_released;
static _Atomic uint64_t g_adds_total;
static _Atomic int g_live, g_live_peak;   // number of registered pools and peak (for the memory readout line)
void nvn_dirty_stat(int *live, int *peak, int *cap) { *live = atomic_load(&g_live); *peak = atomic_load(&g_live_peak); *cap = MAX_RANGE; }
static _Atomic int g_overflow_logged;
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;   // Pool creation / destruction (guest threads, possibly concurrent) pick slots under a mutex; the page-fault handling path does not touch it
static _Atomic uint64_t g_mp_calls, g_mp_pages, g_mp_ticks;   // This module's mprotect: call count, pages covered, time spent (mach timebase, safe in the page-fault path too)
static _Atomic int g_mp_red;
static int mp(uintptr_t a, size_t len, int prot) {
    uint64_t t = mach_absolute_time(); int rc = mprotect((void *)a, len, prot);
    atomic_fetch_add(&g_mp_ticks, mach_absolute_time() - t); atomic_fetch_add(&g_mp_calls, 1); atomic_fetch_add(&g_mp_pages, len >> g_shift);
    if (rc && !atomic_exchange(&g_mp_red, 1)) LOG("[RED] dirty-page tracking: mprotect %p +%#zx failed (the process's mapping limit?)", (void *)a, len);
    return rc;
}

static mach_timebase_info_data_t g_tb;   // obtained when the first pool is registered (the fault path only multiplies and divides)
static _Atomic int g_busy_red;
// Wait for this page to leave BUSY. Returns 1 = it has left; 0 = waited past the limit (reported red once). Usable in a signal handler: only atomic reads, mach_absolute_time and sched_yield
static int busy_wait(range_t *r, uintptr_t p) {
    uint64_t t0 = 0;
    for (unsigned n = 1; atomic_load(&r->state[p]) == PG_BUSY; n++) {
        if (n & 0x3ff) continue;
        uint64_t now = mach_absolute_time(); if (!t0) t0 = now;
        if ((now - t0) * g_tb.numer / g_tb.denom > (uint64_t)BUSY_WAIT_MS * 1000000ull) {
            if (!atomic_exchange(&g_busy_red, 1)) LOG("[RED] dirty-page tracking: page %p stuck 'busy' for over %d ms (the thread changing its protection has not finished), no longer waiting", (void *)(r->base + (p << g_shift)), BUSY_WAIT_MS);
            return 0; }
        sched_yield();
    }
    return 1;
}
static int live_at(int i) { return atomic_load_explicit(&rg(i)->live, memory_order_acquire); }
// The registered range that holds all of [a, a+len) (used for a texture's tracking / protection / dirty check)
static range_t *find_span(uintptr_t a, size_t len) {
    int n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) { range_t *r = rg(i); if (live_at(i) && a >= r->base && a + len <= r->end) return r; }
    return NULL;
}
static int unprotect_page(range_t *r, uintptr_t p) {   // The caller has already changed the state from PROT to BUSY. 0 = unprotected; else it stays protected
    atomic_fetch_add(&r->nmp, 1);
    if (mp(r->base + (p << g_shift), g_pg, PROT_READ | PROT_WRITE)) { atomic_store(&r->state[p], PG_PROT); return -1; }
    stamp_page(r, p);
    atomic_store(&r->state[p], PG_NONE);
    return 0;
}
// Unprotect one page of a range: if protected, unprotect and stamp it; if another thread is changing its protection, wait for it to finish and look again -- if the other side was setting read-only, the page is PROT after the wait and still has to be unprotected
// (an unprotect-before-write never reaches the fault handler, so it must be done here). Returns 1 = this page is managed by this module; 0 = it is not, or the wait on BUSY timed out
static int release_page(range_t *r, uintptr_t p, _Atomic uint64_t *count) {
    for (;;) {
        uint8_t st = PG_PROT;
        if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { if (unprotect_page(r, p)) return 0;   // a write let through is the only way on: not ours, the crash handler reports it
            if (count) atomic_fetch_add(count, 1); return 1; }
        if (st != PG_BUSY) return atomic_load(&r->gen[p]) != 0;   // already NONE: just unprotected by another thread (the fault happened before the unprotect) -> re-execute; pages never protected are not ours
        if (!busy_wait(r, p)) return 0;
    }
}
static void release_pages(range_t *r, uintptr_t p, uintptr_t q, _Atomic uint64_t *count);
static int on_fault(void *addr, int is_write, void *uc) {
    (void)uc;
    if (!is_write) return 0;
    uintptr_t a = (uintptr_t)addr; int ours = 0, n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) {   // Every range sharing the page must be released: releasing only one leaves the other range's state at "protected", and later writes no longer fault and are missed
        range_t *r = rg(i); if (!live_at(i) || a < r->base || a >= r->end) continue;
        uintptr_t pg = (a - r->base) >> g_shift; uint32_t sp = atomic_load(&r->span[pg]);
        ours |= release_page(r, pg, &g_faults);
        // The rest of the storage the same protect call covered goes with it: the texture is uploaded again after any write into it, so this costs no upload, and the
        // game's writes into it fault once instead of once a page. Released a page at a time, a texture written at every other page left its storage in alternating
        // read-only and writable pages, each a mapping of its own: Xenoblade Chronicles 3 reached the kernel's mapping limit (65,530) in its first battle, after which
        // mprotect failed, a faulting write was never let through, and the game hung (or an UnmapPhysicalMemory failed with ENOMEM)
        if (sp) { uintptr_t q = sp - 1; while (q < r->cap && q < ((r->end - r->base) >> g_shift) && atomic_load(&r->span[q]) == sp) q++; if (q > sp - 1) release_pages(r, sp - 1, q - 1, &g_faults); }
    }
    return ours;
}
// Unprotect pages p..q of a range with one mprotect per run of protected pages (a host write over a whole texture, e.g. WriteTexels, used to make one system call per 16 KB page:
// Link's Awakening, about 275 a frame). Pages another thread is changing are handled one by one after the run so far is finished, so no BUSY page is held while waiting
static void release_pages(range_t *r, uintptr_t p, uintptr_t q, _Atomic uint64_t *count) {
    uintptr_t run = 0; int open = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_PROT; int took = i <= q && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);
        if (took) { if (!open) { run = i; open = 1; } continue; }
        if (open) {
            atomic_fetch_add(&r->nmp, 1); mp(r->base + (run << g_shift), (i - run) << g_shift, PROT_READ | PROT_WRITE);
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
        if (!live_at(i) || e <= r->base || a >= r->end) continue;
        uintptr_t lo = a > r->base ? a : r->base, hi = e < r->end ? e : r->end;
        if (count == &g_preps) atomic_store(&r->prep, mach_absolute_time());
        release_pages(r, (lo - r->base) >> g_shift, (hi - 1 - r->base) >> g_shift, count);
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
// runs. Run on the thread that protects textures (the replay's); remap() returns 0 when it mapped the range.
// A range the host prepared a write into in the last DEFRAG_QUIET_MS is left for later: the host's own writes (a file read into a pool, which the game does for its
// streaming) are system calls the kernel fails with EFAULT on a page made read-only under them; Xenoblade Chronicles 3 then inflated a short read and crashed in zlib
enum { DEFRAG_MP = 4000, DEFRAG_QUIET_MS = 1000 };
int nvn_dirty_defrag(int (*remap)(void *mem, size_t len)) {
    int done = 0, n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) {
        range_t *r = rg(i); if (!live_at(i) || atomic_load(&r->nmp) < DEFRAG_MP || r->mem != r->base) continue;   // a pool starting mid-page shares that page: left alone
        if ((mach_absolute_time() - atomic_load(&r->prep)) * g_tb.numer / g_tb.denom < (uint64_t)DEFRAG_QUIET_MS * 1000000ull) continue;
        size_t np = (r->end - r->base) >> g_shift;
        uint8_t *was = malloc(np); if (!was) continue;
        for (size_t p = 0; p < np; p++) {   // every page BUSY; was = its state before
            for (;;) { uint8_t st = atomic_load(&r->state[p]);
                if (st == PG_BUSY) { if (!busy_wait(r, p)) break; continue; }
                if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { was[p] = st; break; } }
        }
        int ok = remap((void *)r->base, r->end - r->base) == 0;
        for (size_t p = 0; ok && p < np; ) {   // the pages that were writable, a run at a time
            if (was[p] != PG_NONE) { p++; continue; }
            size_t q = p; while (q < np && was[q] == PG_NONE) q++;
            mp(r->base + (p << g_shift), (q - p) << g_shift, PROT_READ | PROT_WRITE); p = q;
        }
        for (size_t p = 0; p < np; p++) {
            if (!ok || was[p] == PG_PROT) { atomic_store(&r->state[p], was[p]); continue; }
            stamp_page(r, p); atomic_store(&r->state[p], PG_NONE);
        }
        free(was);
        if (ok) { atomic_store(&r->nmp, 0); done++; }
    }
    return done;
}
// Runs of protected pages over every live range: each is a mapping of its own (at most two with the writable pieces around it)
int nvn_dirty_runs(void) {
    int n = atomic_load(&g_nr), runs = 0;
    for (int i = 0; i < n; i++) {
        range_t *r = rg(i); if (!live_at(i)) continue;
        size_t np = (r->end - r->base) >> g_shift; int prev = 0;
        for (size_t p = 0; p < np; p++) { int pr = atomic_load_explicit(&r->state[p], memory_order_relaxed) != PG_NONE; runs += pr && !prev; prev = pr; }
    }
    return runs;
}

void nvn_dirty_add_range(const void *mem, size_t size) {
    static int inited;
    if (!inited) { inited = 1; g_pg = (uintptr_t)getpagesize(); g_shift = __builtin_ctzl(g_pg); mach_timebase_info(&g_tb); dm_fault_hook_register(on_fault); dm_guest_write_prep_register(prep_write); }
    atomic_fetch_add(&g_adds_total, 1);
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
    uintptr_t base = (uintptr_t)mem & ~(g_pg - 1), end = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
    size_t np = (end - base) >> g_shift;
    _Atomic uint8_t *state = r->state;
    _Atomic uint64_t *gen = r->gen;
    if (r->cap < np) {
        state = calloc(np, 1); gen = calloc(np, sizeof *gen); r->span = calloc(np, sizeof *r->span);
        if (!state || !gen || !r->span) die("dirty-page table allocation failed (%zu pages)", np);
        // The old array is not freed: the signal handler may still have a very brief stale reader; leak = 9 bytes per page.
        r->cap = np;
    } else {
        memset((void *)state, 0, np); memset((void *)gen, 0, np * sizeof *gen); memset((void *)r->span, 0, np * sizeof *r->span);
    }
    r->base = base; r->end = end; r->mem = (uintptr_t)mem; r->state = state; r->gen = gen; atomic_store(&r->nmp, 0); atomic_store(&r->prep, 0); atomic_store(&r->last, 0); atomic_store(&r->blk, NULL);
    if (slot == n) atomic_store(&g_nr, n + 1);
    atomic_store_explicit(&r->live, 1, memory_order_release);
    pthread_mutex_unlock(&g_slot_mu);
}
// Pool destruction: first restore still-protected pages to writable (the pool memory may be reused for something else once returned to the game), then mark invalid
void nvn_dirty_remove_range(const void *mem) {
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = NULL; int n = atomic_load(&g_nr);
    for (int i = 0; i < n && !r; i++) if (live_at(i) && rg(i)->mem == (uintptr_t)mem) r = rg(i);   // Match by the pool's exact start: an adjacent pool sharing a page may have the same rounded base
    if (r) { prep_write((void *)r->base, r->end - r->base); atomic_store_explicit(&r->live, 0, memory_order_release); atomic_fetch_sub(&g_live, 1); }
    pthread_mutex_unlock(&g_slot_mu);
}
int nvn_dirty_tracked(const void *addr, size_t len) {
    return find_span((uintptr_t)addr, len) != NULL;
}
// The tracked range holding [addr, addr+len), as a handle a caller keeps (packaged textures: the scan over every range ran twice per texture lookup). NULL = untracked
const void *nvn_dirty_span(const void *addr, size_t len) { return len ? find_span((uintptr_t)addr, len) : NULL; }
// Set read-only: mark the pages BUSY first and mark them PROT only after mprotect has landed (for the order and the invariant see the state notes at the top of the file)
static void protect_in(range_t *r, const void *addr, size_t len);
void nvn_dirty_protect(const void *addr, size_t len) { range_t *r = len ? find_span((uintptr_t)addr, len) : NULL; if (r) protect_in(r, addr, len); }
void nvn_dirty_protect_span(const void *span, const void *addr, size_t len) {   // through a kept handle, as nvn_dirty_since_span
    range_t *r = (range_t *)span; uintptr_t a = (uintptr_t)addr;
    if (!r || !atomic_load_explicit(&r->live, memory_order_acquire) || a < r->base || a + len > r->end) nvn_dirty_protect(addr, len); else if (len) protect_in(r, addr, len);
}
static void protect_in(range_t *r, const void *addr, size_t len) {
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    // Consecutive pages are merged into one mprotect (per-page calls reach thousands per frame on the first upload of large textures; character select page startup frame 5: 4888 calls).
    // Pages already PROT / BUSY break the consecutive run: do not protect again, and do not touch pages another thread is changing
    uintptr_t run = 0; int open = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_NONE;
        int took = i <= q && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);
        if (took && !open) { run = i; open = 1; }
        if (!took && open) { atomic_fetch_add(&r->nmp, 1); mp(r->base + (run << g_shift), (i - run) << g_shift, PROT_READ); for (uintptr_t k = run; k < i; k++) { atomic_store(&r->span[k], (uint32_t)p + 1); atomic_store(&r->state[k], PG_PROT); } open = 0; }
    }
}
uint64_t nvn_dirty_now(void) { return atomic_load(&g_now); }
int nvn_dirty_since_span(const void *span, const void *addr, size_t len, uint64_t stamp) {
    if (!len) return 0;
    range_t *r = (range_t *)span; uintptr_t a0 = (uintptr_t)addr;   // the kept handle still holds the span (a slot reused by another pool over the same memory is as good): no scan
    if (!r || !atomic_load_explicit(&r->live, memory_order_acquire) || a0 < r->base || a0 + len > r->end) r = find_span(a0, len);
    if (!r) return 1;   // When untracked, conservatively treat as dirty: better to upload more than to show stale content
    if (atomic_load(&r->last) <= stamp) return 0;
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    for (uintptr_t i = p; i <= q; i++) if (atomic_load(&r->gen[i]) > stamp) return 1;   // The generation of the last write before upload is <= the stamp; any later write must be > the stamp
    return 0;
}
// Write tracking by block for a caller that copies the range elsewhere: each page made writable from now on sets its blocks' bits (1 << shift bytes each, counted from
// the pool's start), which the caller takes word by word. A write lets the page through only after its bits are set, so a block whose bit was taken and found clear,
// and was not protected again, is written later with its bit set again
int nvn_dirty_blocks(const void *span, unsigned shift) {
    range_t *r = (range_t *)span; if (!r) return -1;
    if (atomic_load(&r->blk)) return r->blk_shift == shift ? 0 : -1;
    pthread_mutex_lock(&g_slot_mu);
    if (!atomic_load(&r->blk)) { size_t n = (((r->end - r->mem + (1ull << shift) - 1) >> shift) + 63) / 64; _Atomic uint64_t *b = calloc(n, sizeof *b);   // never freed: the fault path may still hold it
        if (b) { r->blk_shift = shift; r->nblk_words = n; atomic_store_explicit(&r->blk, b, memory_order_release); } }
    pthread_mutex_unlock(&g_slot_mu);
    return atomic_load(&r->blk) && r->blk_shift == shift ? 0 : -1;
}
uint64_t nvn_dirty_blocks_take(const void *span, size_t word) {
    const range_t *r = span; _Atomic uint64_t *b = r ? atomic_load_explicit(&r->blk, memory_order_acquire) : NULL;
    return b && word < r->nblk_words ? atomic_exchange(&b[word], 0) : ~0ull;   // (no bitmap: every block written)
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
