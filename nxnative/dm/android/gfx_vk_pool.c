// Android port, Vulkan backend: memory pools (see gfx_vk.h). NVN gives the GPU the game's own memory (GPU address = CPU address). Each pool's pages are
// replaced by an AHardwareBuffer mapped at the same address (its dma-buf mmapped MAP_FIXED, contents copied over), and the same AHardwareBuffer is imported
// as a VkBuffer: the game's pointer is GPU memory, CPU-cached and coherent on Adreno (measured: probes/vkzc.c). A pool the allocator refuses keeps its
// anonymous pages and gets a GPU copy refreshed before every submission (vk_pools_sync) and written back after it (vk_pools_writeback), reported once.
#include "gfx_vk.h"
#include "nvn_dirty.h"
#include <android/hardware_buffer.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAXPOOL 4096
static vkpool_t g_pool[MAXPOOL]; static int g_npool;
static pthread_rwlock_t g_pool_mu = PTHREAD_RWLOCK_INITIALIZER;
static int g_ncopied;   // pools with a GPU copy (shadow)
enum { SYNC_BLK = 64 << 10 };   // a copied pool's incremental sync works in blocks of this size
// A sequence lock over g_pool for vk_pool_of, which takes no lock: odd while a writer (under the write lock) changes the pools, bumped twice per change
static uint32_t g_pool_gen;
static void pool_change(void) { __atomic_fetch_add(&g_pool_gen, 1, __ATOMIC_RELEASE); __atomic_thread_fence(__ATOMIC_SEQ_CST); }
// g_pool's indices by lo and their lo, remade whenever a pool is added or removed: vk_pool_of's binary search reads the dense lo array alone, not the pool entries
// (120 bytes each). A linear scan of g_pool was 18% of the profiler's samples of Super Mario Bros. Wonder's replay in its first level; with a search its world map's
// replay took 4.2 ms a frame instead of 4.6-4.9
static uint16_t g_pool_ord[MAXPOOL]; static uint64_t g_ord_lo[MAXPOOL];
static int pool_ord_cmp(const void *a, const void *b) { uint64_t x = g_pool[*(const uint16_t *)a].lo, y = g_pool[*(const uint16_t *)b].lo; return x < y ? -1 : x > y; }
static void pool_ord_make(void) {
    for (int i = 0; i < g_npool; i++) g_pool_ord[i] = (uint16_t)i;
    qsort(g_pool_ord, (size_t)g_npool, sizeof *g_pool_ord, pool_ord_cmp);
    for (int i = 0; i < g_npool; i++) g_ord_lo[i] = g_pool[g_pool_ord[i]].lo;
}
static int recv_fds(int sock, int *fds, int max) {
    char data[4096], ctl[CMSG_SPACE(sizeof(int) * 16)]; struct iovec iov = {data, sizeof data};
    struct msghdr m = {0}; m.msg_iov = &iov; m.msg_iovlen = 1; m.msg_control = ctl; m.msg_controllen = sizeof ctl;
    if (recvmsg(sock, &m, 0) < 0) return -1;
    int n = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
            for (size_t i = 0; i < (c->cmsg_len - CMSG_LEN(0)) / sizeof(int) && n < max; i++) fds[n++] = ((int *)CMSG_DATA(c))[i];
    return n;
}
// a zero-copy pool's buffer also has a device address, which shaders storing through pointers they loaded reach it by (vk_gmem_table); its memory is allocated for one
// (pool_mem_flags). Copied pools have none: on the Android emulator, host-visible memory allocated for a device address took a hypervisor mapping slot each, and entering
// Donkey Kong Bananza's first level ran out of them ("hvf_map_safe: no free slots", the emulator died)
#define POOL_USAGE_BDA (VK_POOL_USAGE | (V.has_bda ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0))
static VkMemoryAllocateFlagsInfo g_bda_flags = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
static const void *pool_mem_flags(const void *next) { if (!V.has_bda) return next; g_bda_flags.pNext = next; return &g_bda_flags; }   // (allocations are made under g_pool_mu or before)
static uint64_t buf_gpu(VkBuffer b) { return V.has_bda ? vkGetBufferDeviceAddress(V.dev, &(VkBufferDeviceAddressInfo){VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, b}) : 0; }
// Zero copy: the pages of [mem, mem+size) become an AHardwareBuffer's, imported as a VkBuffer. 0 on success
static int pool_zero_copy(vkpool_t *p, uint8_t *mem, uint64_t size) {
    AHardwareBuffer_Desc d = {(uint32_t)size, 1, 1, AHARDWAREBUFFER_FORMAT_BLOB,
        AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER, 0, 0, 0};
    if (size > 0xffffffffull) return -1;
    AHardwareBuffer *ahb; if (AHardwareBuffer_allocate(&d, &ahb)) return -1;
    int sv[2]; if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) { AHardwareBuffer_release(ahb); return -1; }
    int fds[16], nfd = -1;
    if (!AHardwareBuffer_sendHandleToUnixSocket(ahb, sv[0])) nfd = recv_fds(sv[1], fds, 16);
    close(sv[0]); close(sv[1]);
    if (nfd < 1) { AHardwareBuffer_release(ahb); return -1; }
    // copy the pool's current contents into the buffer through a temporary mapping, then put the buffer's pages at the pool's address
    uint8_t *tmp = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fds[0], 0);
    int ok = tmp != MAP_FAILED;
    if (ok) { memcpy(tmp, mem, size); munmap(tmp, size); }
    ok = ok && mmap(mem, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fds[0], 0) == mem;
    for (int i = 0; i < nfd; i++) close(fds[i]);
    if (!ok) { AHardwareBuffer_release(ahb); return -1; }
    VkAndroidHardwareBufferPropertiesANDROID ap = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb, 0, size, POOL_USAGE_BDA, VK_SHARING_MODE_EXCLUSIVE};
    if (vkGetAndroidHardwareBufferPropertiesANDROID(V.dev, ahb, &ap) || vkCreateBuffer(V.dev, &bi, NULL, &p->buf)) { AHardwareBuffer_release(ahb); return -2; }
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, p->buf};   // the driver requires a dedicated allocation for an imported AHardwareBuffer
    VkImportAndroidHardwareBufferInfoANDROID imp = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, &ded, ahb};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, pool_mem_flags(&imp), ap.allocationSize, vk_mem_type(ap.memoryTypeBits, 0, 0)};
    if (vkAllocateMemory(V.dev, &ai, NULL, &p->mem) || vkBindBufferMemory(V.dev, p->buf, p->mem, 0)) { vkDestroyBuffer(V.dev, p->buf, NULL); AHardwareBuffer_release(ahb); return -2; }
    p->ahb = ahb; p->zero_copy = 1; p->gpu = buf_gpu(p->buf);
    return 0;
}
// A zero-copy pool's dma-buf mapped again over [mem, mem+len) in one piece per chunk, read-only (nvn_dirty_defrag: the mappings its protection split never merge). 0 = done
int vk_pool_remap_ro(void *mem, size_t len) {
    int rc = 0; uint64_t done = 0;
    pthread_rwlock_rdlock(&g_pool_mu);
    for (int i = 0; i < g_npool && !rc; i++) {
        vkpool_t *p = &g_pool[i]; if (p->base != (uint64_t)(uintptr_t)mem || p->end - p->base != len) continue;
        if (!p->zero_copy) { if (!p->shadow) { rc = -1; break; } done += p->hi - p->lo; continue; }   // a copied pool: nothing to map again (its protection stays as it is), and done, so defrag stops taking its pages busy
        int sv[2], fds[16], nfd = -1; if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) { rc = -1; break; }
        if (!AHardwareBuffer_sendHandleToUnixSocket(p->ahb, sv[0])) nfd = recv_fds(sv[1], fds, 16);
        close(sv[0]); close(sv[1]);
        void *at = (void *)(uintptr_t)p->lo; rc = nfd >= 1 && mmap(at, p->hi - p->lo, PROT_READ, MAP_SHARED | MAP_FIXED, fds[0], 0) == at ? 0 : -1;
        for (int k = 0; k < nfd; k++) close(fds[k]);
        if (!rc) done += p->hi - p->lo;
    }
    pthread_rwlock_unlock(&g_pool_mu);
    return !rc && done == len ? 0 : -1;
}
static void pool_copy_back(vkpool_t *p) {   // the pages go back to anonymous memory with the buffer's contents (the game may reuse the memory)
    uint64_t size = p->hi - p->lo; uint8_t *mem = (uint8_t *)(uintptr_t)p->lo;
    uint8_t *save = malloc(size); if (save) memcpy(save, mem, size);
    mmap(mem, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (save) { memcpy(mem, save, size); free(save); }
}
// Pools over ZC_CHUNK are made of chunks of at most that size, each a pool entry with an AHardwareBuffer of its own: the Adreno 830's gralloc allocator service
// (vendor.qti.hardware.display.allocator-service, snapalloc) aborts on a BLOB of 812 MB ("ubsan: mul-overflow" in SnapConstraintManager::AlignmentToAlignedConstraints),
// and the whole Android UI restarts with it; 400 MB ones work (13 Sentinels: Aegis Rim's 0x32c00000-byte pool). A range across two chunks has no buffer (vk_pool_of)
#define ZC_CHUNK 0x10000000ull
static void pool_add_one(const void *mem, uint64_t size, uint64_t base, uint64_t end);
// DM_VK_ZEROCOPY_MAX_MB=<n>: pools bigger than n MB are copied, not zero-copy, and kept whole (a copied pool takes no hardware buffer, so no chunks). The Legion Y700
// 2023's gralloc aborts the process (no error returned) allocating a hardware buffer for Unicorn Overlord's 860 MB pool, while its pools up to 250 MB are fine
static int zc_over(uint64_t size) { static uint64_t m = 1; if (m == 1) { const char *e = getenv("DM_VK_ZEROCOPY_MAX_MB"); m = e && atoi(e) > 0 ? (uint64_t)atoi(e) << 20 : 0; } return m && size > m; }
void mtl_pool_add(const void *mem, uint64_t size) {
    vk_init();
    if (!size) return;
    uint64_t a = (uint64_t)(uintptr_t)mem, c = ((a | size) & 0xfff) || zc_over(size) ? size : ZC_CHUNK;
    for (uint64_t o = 0; o < size; o += c) pool_add_one((const uint8_t *)mem + o, size - o < c ? size - o : c, a, a + size);
    if (size > c) VKLOG("memory pool %p +%#llx: %llu chunks of at most %#llx", mem, (unsigned long long)size, (unsigned long long)((size + c - 1) / c), c);
}
static void pool_add_one(const void *mem, uint64_t size, uint64_t base, uint64_t end) {
    pthread_rwlock_wrlock(&g_pool_mu);
    if (g_npool == MAXPOOL) { pthread_rwlock_unlock(&g_pool_mu); die("[vk] too many memory pools"); }
    vkpool_t *p = &g_pool[g_npool]; memset(p, 0, sizeof *p);
    p->lo = (uint64_t)(uintptr_t)mem; p->hi = p->lo + size; p->base = base; p->end = end; { static uint64_t ser; p->serial = ++ser; }
    int zc = ((uintptr_t)mem & 0xfff) || (size & 0xfff) ? -3 : getenv("DM_VK_NO_ZEROCOPY") ? -4 : zc_over(end - base) ? -5 : pool_zero_copy(p, (uint8_t *)mem, size);
    if (zc) {
        static int said; if (!said++) VKLOG("[RED] memory pool %p +%#llx could not be zero-copy (%d): a GPU copy is refreshed before each submission (slow)", mem, (unsigned long long)size, zc);
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, VK_POOL_USAGE, VK_SHARING_MODE_EXCLUSIVE};
        VKCHK(vkCreateBuffer(V.dev, &bi, NULL, &p->buf));
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, p->buf, &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
            vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
        VKCHK(vkAllocateMemory(V.dev, &ai, NULL, &p->mem)); VKCHK(vkBindBufferMemory(V.dev, p->buf, p->mem, 0));
        void *m; VKCHK(vkMapMemory(V.dev, p->mem, 0, VK_WHOLE_SIZE, 0, &m)); p->shadow = m;
        if (!(p->hot = calloc((size + SYNC_BLK - 1) / SYNC_BLK, 1))) die("[vk] out of memory");
        __atomic_fetch_add(&g_ncopied, 1, __ATOMIC_RELAXED);
    }
    pool_change(); g_npool++; pool_ord_make(); pool_change();
    pthread_rwlock_unlock(&g_pool_mu);
    static uint64_t zc_bytes, cp_bytes; if (zc) cp_bytes += size; else zc_bytes += size;
    static int nlog; if (nlog++ < 24) VKLOG("memory pool %p +%#llx: %s (zero-copy %.1f MB, copied %.1f MB)", mem, (unsigned long long)size, zc ? "copied" : "zero-copy", zc_bytes / 1048576.0, cp_bytes / 1048576.0);
}
void mtl_pool_del(const void *mem) {
    for (;;) {   // every chunk of the pool
        pthread_rwlock_wrlock(&g_pool_mu);
        int i = 0; while (i < g_npool && g_pool[i].base != (uint64_t)(uintptr_t)mem) i++;
        if (i == g_npool) { pthread_rwlock_unlock(&g_pool_mu); return; }
        vkpool_t p = g_pool[i]; pool_change(); g_pool[i] = g_pool[--g_npool]; pool_ord_make(); pool_change();
        pthread_rwlock_unlock(&g_pool_mu);
        if (p.zero_copy) pool_copy_back(&p);
        if (p.shadow) __atomic_fetch_sub(&g_ncopied, 1, __ATOMIC_RELAXED);
        free(p.hot); vk_defer_free_buffer(p.buf, p.mem);   // the AHardwareBuffer goes with the imported memory's last reference
        if (p.ahb) AHardwareBuffer_release(p.ahb);
    }
}
// The memory pool table that global accesses through loaded pointers search (GlobalToStorage.cs TryGeneratePhysicalStorageOp), in the open command buffer's scratch:
// words 0 the entry count, 2 / 3 the sink's device address (an address in no pool reads and writes there); from 4, 8 words a pool by start: start, end, device minus
// guest address (lo, hi each). Built again when the pools change or a submission begins (its scratch is the submission's)
void vk_gmem_table(VkBuffer *buf, VkDeviceSize *off, VkDeviceSize *size) {
    static VkBuffer tb; static VkDeviceSize to, tn; static uint32_t gen; static uint64_t sub, sink;
    if (!V.has_bda) VKRED_ONCE(90, "a shader stores through pointers it loads, and the device has no buffer device address: its accesses go nowhere");
    static int made; if (!made++ && V.has_bda) {   // 4 KB of device memory for addresses in no pool
        VkBuffer b; VkDeviceMemory m; VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_SHARING_MODE_EXCLUSIVE};
        VKCHK(vkCreateBuffer(V.dev, &bi, NULL, &b)); VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, b, &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, pool_mem_flags(NULL), req.size, vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
        VKCHK(vkAllocateMemory(V.dev, &ai, NULL, &m)); VKCHK(vkBindBufferMemory(V.dev, b, m, 0)); sink = buf_gpu(b);
        VKLOG("global accesses through loaded pointers: sink at device address %#llx", (unsigned long long)sink);
    }
    uint32_t g = __atomic_load_n(&g_pool_gen, __ATOMIC_ACQUIRE);
    if (tb && gen == g && sub == mtl_last_fence()) { *buf = tb; *off = to; *size = tn; return; }
    pthread_rwlock_rdlock(&g_pool_mu);
    tn = 16 + 32 * (VkDeviceSize)(g_npool ? g_npool : 1);
    uint32_t *w = vk_scratch(tn, V.props.limits.minStorageBufferOffsetAlignment, &tb, &to); memset(w, 0, tn);
    uint32_t n = 0;
    for (int k = 0; k < g_npool; k++) { const vkpool_t *p = &g_pool[g_pool_ord[k]]; if (!p->gpu) continue;
        uint64_t e[4] = {p->lo, p->hi, p->gpu - p->lo, 0}; memcpy(w + 4 + 8 * n++, e, sizeof e); }
    pthread_rwlock_unlock(&g_pool_mu);
    w[0] = n ? n : 1; memcpy(w + 2, &sink, 8);
    gen = g; sub = mtl_last_fence(); *buf = tb; *off = to; *size = tn;
}
static vkpool_t *pool_find(uint64_t addr, uint64_t len, int *across) {
    int n = g_npool; const uint64_t *b = g_ord_lo;
    if (n && b[0] <= addr) {   // the last pool by lo that starts at or below addr
        while (n > 1) { int h = n / 2; if (b[h] <= addr) b += h; n -= h; }
        vkpool_t *p = &g_pool[g_pool_ord[b - g_ord_lo]]; if (addr + len <= p->hi) return p; }
    for (int i = 0; i < g_npool; i++) if (addr >= g_pool[i].lo && addr + len <= g_pool[i].hi) return &g_pool[i];   // pools over the same memory
                                       else if (addr >= g_pool[i].lo && addr < g_pool[i].hi && addr + len <= g_pool[i].end) *across = 1;
    return NULL;
}
// The replay looks up every buffer binding of every draw: a locked search of the pool entries behind a 4-entry per-thread cache was 5.5% of Donkey Kong Bananza's
// replay thread on the Android emulator, this search of the dense array alone 2.9% (the cache's per-thread storage access cost more than it saved)
vkpool_t *vk_pool_of(uint64_t addr, uint64_t len) {
    // DM_VK_AB & AB_POOL_TLS: the last 4 pools this thread found first (they hold while the pools are unchanged), as the search's locked path once needed
    static __thread struct { uint32_t gen; vkpool_t *p[4]; } c;
    int tls = g_vk_ab & AB_POOL_TLS; uint32_t g;
    if (tls && c.gen == (g = __atomic_load_n(&g_pool_gen, __ATOMIC_ACQUIRE)) + 1) for (int i = 0; i < 4; i++) { vkpool_t *p = c.p[i]; if (p && addr >= p->lo && addr + len <= p->hi) return p; }
    vkpool_t *r; int across;
    for (;;) {   // the sequence lock's read: searched again when a pool was added or removed meanwhile
        while ((g = __atomic_load_n(&g_pool_gen, __ATOMIC_ACQUIRE)) & 1) {}
        across = 0; r = pool_find(addr, len, &across);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g_pool_gen, __ATOMIC_RELAXED) == g) break;
    }
    if (r && tls) { if (c.gen != g + 1) { memset(&c, 0, sizeof c); c.gen = g + 1; } memmove(&c.p[1], &c.p[0], 3 * sizeof *c.p); c.p[0] = r; }
    if (!r && across) { static uint64_t n; uint64_t k = ++n; if (!(k & (k - 1))) LOG("[RED] [vk] %llu ranges across two chunks of a memory pool (ZC_CHUNK; this one %#llx +%#llx): no buffer for them", (unsigned long long)k, (unsigned long long)addr, (unsigned long long)len); }
    return r;
}
uint32_t vk_pool_gen(void) { return __atomic_load_n(&g_pool_gen, __ATOMIC_ACQUIRE); }   // pools added or removed (gfx_vk_exec.c's descriptor set memo: vk_pool_of's results hold while it is unchanged)
int vk_buf_at(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    vkpool_t *p = vk_pool_of(addr, len ? len : 1);
    if (!p) return 0;
    *buf = p->buf; *off = addr - p->lo; return 1;
}
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    vkpool_t *p = vk_pool_of(addr, len ? len : 1);
    if (!p) return 0;
    if (buf) *buf = (void *)(uintptr_t)p->lo; if (off) *off = addr - p->lo; return 1;
}
void mtl_alias_drop_pool(const void *pool) { void vk_tex_drop_pool(const void *); vk_tex_drop_pool(pool); }
// Copied pools: the guest's bytes into the GPU copy, before each submission. The first sync copies the whole pool; later ones copy only the blocks with pages the
// CPU wrote since the sync before the last (nvn_dirty's write-protect tracking: a block is protected again before it is copied, so a write during the copy is
// caught by the next sync). Judged against the sync before the last: a write whose fault took its generation before the last sync's stamp but stored it after
// that sync looked at its block would otherwise never count. An 860 MB pool copied whole at every submission (Unicorn Overlord on a Legion Y700 2023) ran at a
// few frames a second.
// A block written in two of its last 7 syncs turns hot: it stays writable and is copied at every sync, and is protected again once in 256 syncs to see whether
// it cooled. The game's per-frame data otherwise took a write-protection fault and an mprotect per block and sync (Donkey Kong Bananza: ~1,000 mprotects a
// frame at ~23 us each on the Android emulator, holding the memory map lock against every other thread's page faults).
// GPU writes of submissions still in flight (written back by the completion thread) must not be overwritten by older guest pages: an untracked or hot block
// waits for them, any other copies only the pages the CPU wrote (a page both wrote in the meantime would be a race on hardware as well)
static uint64_t g_sync_bytes, g_sync_full, g_sync_ns;
static uint64_t gw_fence(const vk_gwrite *w, int n, uint64_t lo, uint64_t hi) {   // the last in-flight submission writing into [lo, hi), 0: none
    uint64_t f = 0, done = vk_fence_done();
    for (int k = 0; k < n; k++) if (w[k].lo < hi && lo < w[k].hi && w[k].fence > done && w[k].fence > f) f = w[k].fence;
    return f;
}
static void pool_sync(vkpool_t *p, const vk_gwrite *gw, int ngw) {
    static uint64_t pgsz; if (!pgsz) pgsz = (uint64_t)getpagesize();
    uint8_t *lo = (uint8_t *)(uintptr_t)p->lo; uint64_t size = p->hi - p->lo, st = nvn_dirty_now();
    if (!p->span && (p->span = nvn_dirty_span(lo, size))) p->synced = 0;   // tracked only now (nvnMemoryPoolInitialize registers the range after the pool): copied whole once more, protected
    if (!p->synced || !p->span) {   // untracked: every sync copies all of it
        if (p->span) nvn_dirty_protect(lo, size);
        uint64_t f = gw_fence(gw, ngw, p->lo, p->hi); if (f) mtl_wait(f, ~0ull);
        memcpy(p->shadow, lo, size); g_sync_bytes += size; g_sync_full++; p->synced = 1; p->stamp0 = p->stamp = st; return; }
    p->nsync++;
    for (uint64_t off = 0, b = 0; off < size; off += SYNC_BLK, b++) {
        uint64_t n = size - off < SYNC_BLK ? size - off : SYNC_BLK; uint8_t *h = &p->hot[b]; int hot = *h >> 7;   // hot[b]: bit 7 hot, bits 0-6 the dirty history
        int dirty = hot || nvn_dirty_since_span(p->span, lo + off, n, p->stamp0), fresh = hot || (dirty && nvn_dirty_since_span(p->span, lo + off, n, p->stamp));
        uint8_t hist = (uint8_t)((*h << 1 | fresh) & 0x7f);   // (a write since the last sync: one write is dirty for two)
        if (!dirty) { *h = hist; continue; }
        uint64_t f = gw_fence(gw, ngw, p->lo + off, p->lo + off + n);
        if (f && hot) { mtl_wait(f, ~0ull); f = 0; hot = 0; }
        else if (!hot) hot = !f && __builtin_popcount(hist) >= 2;
        else hot = (p->nsync & 255) != (b & 255);
        *h = (uint8_t)(hist | hot << 7);
        if (!hot) nvn_dirty_protect(lo + off, n);
        if (!f) { memcpy(p->shadow + off, lo + off, n); g_sync_bytes += n; continue; }
        for (uint64_t q = off; q < off + n; q += pgsz) if (nvn_dirty_since_span(p->span, lo + q, pgsz, p->stamp0)) { memcpy(p->shadow + q, lo + q, pgsz); g_sync_bytes += pgsz; }
    }
    p->stamp0 = p->stamp; p->stamp = st;
}
void vk_pools_sync(void) {
    if (!__atomic_load_n(&g_ncopied, __ATOMIC_RELAXED)) return;
    extern int g_prof_on; extern uint64_t g_prof_ns[8]; struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    vk_gwrite gw[VK_GWRITE_MAX]; int ngw = vk_inflight_writes(gw);
    pthread_rwlock_rdlock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) if (g_pool[i].shadow) pool_sync(&g_pool[i], gw, ngw);
    pthread_rwlock_unlock(&g_pool_mu);
    clock_gettime(CLOCK_MONOTONIC, &b); uint64_t d = (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec)); g_sync_ns += d; if (g_prof_on) g_prof_ns[2] += d;
    static uint64_t n, last, last_ns; if (++n % 600 == 0) { VKLOG("copied pools: %.1f MB synced in the last 600 submissions (%.2f MB, %.2f ms each), %llu full copies so far", (g_sync_bytes - last) / 1048576.0, (g_sync_bytes - last) / 600.0 / 1048576.0, (g_sync_ns - last_ns) / 600.0 / 1e6, (unsigned long long)g_sync_full); last = g_sync_bytes; last_ns = g_sync_ns; }
}
// Copied pools after a submission: the ranges its GPU writes covered, back into the guest's pages (a pool never synced yet holds nothing of the game's). Copying
// whole pools back held Unicorn Overlord to a few frames a second, and wrote every page of them, which the next sync then copied to the GPU again: 1.4 GB a
// submission in Donkey Kong Bananza. Written through /proc/self/mem, which a page's write protection does not stop: the pages take no fault and no new
// generation, so the next sync does not copy them again and a block the GPU writes does not turn hot. memcpy when the kernel refuses
void vk_pools_writeback(const vk_range *gw, int ngw) {
    if (!ngw || !__atomic_load_n(&g_ncopied, __ATOMIC_RELAXED)) return;
    static int fd = -2; if (fd == -2) fd = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
    pthread_rwlock_rdlock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) { vkpool_t *p = &g_pool[i]; if (!p->shadow || !p->synced) continue;
        for (int k = 0; k < ngw; k++) { uint64_t lo = gw[k].lo > p->lo ? gw[k].lo : p->lo, hi = gw[k].hi < p->hi ? gw[k].hi : p->hi;
            if (lo < hi && (fd < 0 || pwrite(fd, p->shadow + (lo - p->lo), hi - lo, (off_t)lo) != (ssize_t)(hi - lo))) memcpy((void *)(uintptr_t)lo, p->shadow + (lo - p->lo), hi - lo); } }
    pthread_rwlock_unlock(&g_pool_mu);
}
