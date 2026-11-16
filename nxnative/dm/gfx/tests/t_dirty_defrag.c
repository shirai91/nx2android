// nvn_dirty_defrag after remap(): every page keeps a state that matches its protection. A range of 4 pages, page 1 protected (a texture), the others writable;
// remap() is a stand-in for gfx_vk_core.c vk_pool_remap_ro that maps all, a part or nothing of the range read-only (here with mprotect), and mprotect can be made
// to fail. Checked per page: its state, and whether a host write into it works (read() from /dev/zero into the page: EFAULT when it is read-only).
//   success (0)            the writable pages are writable again and NONE, the protected page stays PROT
//   nothing mapped (1)     no protection changes, no mprotect call, every state as before (a pool not whole host pages: its range is rounded up, remap cannot take it)
//   a part mapped (-1)     as after a success: the writable runs it made read-only are writable again
//   restore fails          a run mprotect cannot make writable again stays read-only and is PROT (its next write is let through), never NONE and read-only
#define mprotect t_mprotect
#include "../nvn/nvn_dirty.c"
#undef mprotect
int mprotect(void *, size_t, int);
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>

// the hooks nvn_dirty_add_range registers (host/crash.c in the game; host writes are let in by calling prep_write below)
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc)) { (void)fn; }
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len)) { (void)fn; }

static int g_fail, g_t_mp, g_mp_fail_rw;
#define EXPECT(name, want, got) do { \
    long long want_ = (long long)(want), got_ = (long long)(got); \
    printf("%-64s expected %-4lld → got %-4lld %s\n", name, want_, got_, want_ == got_ ? "✓" : "✗"); \
    if (want_ != got_) g_fail++; \
} while (0)
int t_mprotect(void *a, size_t n, int prot) {
    g_t_mp++;
    if (g_mp_fail_rw && (prot & PROT_WRITE)) { errno = ENOMEM; return -1; }
    return mprotect(a, n, prot);
}
static uint8_t *g_mem; static size_t g_pg; static int g_remap_rc, g_remap_pages;
static int remap(void *mem, size_t len) {   // maps g_remap_pages pages from mem read-only, then answers g_remap_rc
    (void)len; if (g_remap_pages) mprotect(mem, (size_t)g_remap_pages * g_pg, PROT_READ); return g_remap_rc;
}
static int writable(int p) {   // a host write into page p works
    int fd = open("/dev/zero", O_RDONLY); ssize_t r = read(fd, g_mem + (size_t)p * g_pg, 16); close(fd); return r == 16;
}
static void setup(void) {   // the range's page 1 protected, the others writable, and fragmented enough for nvn_dirty_defrag to take it
    nvn_dirty_release(g_mem, 4 * g_pg); nvn_dirty_protect(g_mem + g_pg, g_pg);
    range_t *r = find_span((uintptr_t)g_mem, 4 * g_pg); atomic_store(&r->nmp, DEFRAG_MP); atomic_store(&r->prep, 0);
}
static int state(int p) { return atomic_load(&find_span((uintptr_t)g_mem, 4 * g_pg)->state[p]); }
// every page's state matches its protection: NONE and writable, or PROT (and then not written through by a host write without prep_write)
static void check(const char *what, const int want[4]) {
    char name[96];
    for (int p = 0; p < 4; p++) {
        snprintf(name, sizeof name, "%s: page %d state", what, p); EXPECT(name, want[p], state(p));
        snprintf(name, sizeof name, "%s: page %d writable", what, p); EXPECT(name, want[p] == PG_NONE, writable(p));
    }
}

int main(void) {
    g_pg = (size_t)getpagesize();
    g_mem = mmap(NULL, 4 * g_pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_mem == MAP_FAILED) { perror("mmap"); return 2; }
    nvn_dirty_add_range(g_mem, 4 * g_pg);
    const int one_prot[4] = {PG_NONE, PG_PROT, PG_NONE, PG_NONE};

    setup(); g_remap_rc = 0; g_remap_pages = 4;
    EXPECT("success: ranges done", 1, nvn_dirty_defrag(remap));
    check("success", one_prot);

    setup(); g_remap_rc = 1; g_remap_pages = 0; g_t_mp = 0;
    EXPECT("nothing mapped: ranges done", 0, nvn_dirty_defrag(remap));
    EXPECT("nothing mapped: mprotect calls", 0, g_t_mp);
    check("nothing mapped", one_prot);
    EXPECT("nothing mapped: tried again only after DEFRAG_MP more", 0, atomic_load(&find_span((uintptr_t)g_mem, 4 * g_pg)->nmp));

    setup(); g_remap_rc = -1; g_remap_pages = 3;
    EXPECT("a part mapped: ranges done", 0, nvn_dirty_defrag(remap));
    check("a part mapped", one_prot);

    setup(); g_remap_rc = -1; g_remap_pages = 4; g_mp_fail_rw = 1;
    EXPECT("restore fails: ranges done", 0, nvn_dirty_defrag(remap));
    g_mp_fail_rw = 0;
    const int all_prot[4] = {PG_PROT, PG_PROT, PG_PROT, PG_PROT};
    check("restore fails", all_prot);
    prep_write(g_mem, 4 * g_pg);   // a host write into them (as a fault would) is let through
    EXPECT("restore fails: a host write afterwards is let through", 1, writable(0) && writable(3) && state(0) == PG_NONE);

    printf("t_dirty_defrag: %s\n", g_fail ? "FAILED" : "all passed"); return g_fail != 0;
}
