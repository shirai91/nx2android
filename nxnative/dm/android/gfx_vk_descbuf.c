// Descriptor buffers (VK_EXT_descriptor_buffer, DM_VK_DESCBUF=1 at start-up), as Eden uses them where the driver has them: a draw's descriptors are written as
// bytes into a buffer the backend owns (vkGetDescriptorEXT) and bound as an offset into it (vkCmdSetDescriptorBufferOffsetsEXT), instead of a descriptor set
// allocated from a pool, written and bound. Adreno copies a bound set's descriptors into the command stream: FINAL FANTASY RESONANCE's heavy submissions took
// ~3,400 16 KB blocks of command memory each (about 6.6 KB a draw), which with 16 slots in flight reached the kernel's mapping limit (gfx_vk_core.c MAPS_*).
// gfx_vk_exec.c keeps collecting a stage's descriptors as VkWriteDescriptorSet and caching them per submission; here a "set" is the offset of its bytes in
// the ring plus one, carried as the VkDescriptorSet value, so the per-submission cache, the per-stage memo and the bind cache work unchanged.
// What it takes: set layouts and pipelines made for descriptor buffers (gfx_vk_shader.c shader_layout, gfx_vk_exec.c build_pipeline / compute_build), no
// dynamic uniform buffers (the offset is in the descriptor), and every buffer's device address: in this mode every buffer is made with
// SHADER_DEVICE_ADDRESS and every allocation with the device-address flag (the wrappers below), and texel buffers are given by address and format, which
// the vkCreateBufferView wrapper keeps for each view.
#include "gfx_vk.h"
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>

int g_db_on;   // descriptor buffers in use
PFN_vkCmdBindDescriptorBuffersEXT vkCmdBindDescriptorBuffersEXT; PFN_vkCmdSetDescriptorBufferOffsetsEXT vkCmdSetDescriptorBufferOffsetsEXT;   // (through gfx_vk_worker.c when it runs)
static PFN_vkGetDescriptorSetLayoutSizeEXT p_layout_size; static PFN_vkGetDescriptorSetLayoutBindingOffsetEXT p_binding_off; static PFN_vkGetDescriptorEXT p_get;
static PFN_vkGetBufferDeviceAddress p_bda;
static VkPhysicalDeviceDescriptorBufferPropertiesEXT g_dbp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};

// ---- every buffer with a device address, every texel buffer view remembered ----
static PFN_vkCreateBuffer r_create_buffer; static PFN_vkDestroyBuffer r_destroy_buffer; static PFN_vkAllocateMemory r_alloc; static PFN_vkCreateBufferView r_create_bview; static PFN_vkDestroyBufferView r_destroy_bview;
typedef struct { uint64_t k; uint64_t a, b, c, d; } dbent;   // buffers: {handle, size, address (0 = not asked yet)}; views: {handle, buffer, offset, range, format}
typedef struct { dbent *e; uint32_t cap, n; pthread_mutex_t mu; } dbmap;
static dbmap g_bufs = {.mu = PTHREAD_MUTEX_INITIALIZER}, g_views = {.mu = PTHREAD_MUTEX_INITIALIZER};
static uint32_t h64(uint64_t k, uint32_t cap) { return (uint32_t)((k * 0x9e3779b97f4a7c15ull) >> 40) & (cap - 1); }
static dbent *map_find(dbmap *m, uint64_t k) {   // under m->mu
    if (!m->cap) return NULL;
    for (uint32_t i = h64(k, m->cap), j = 0; j < m->cap && m->e[i].k; j++, i = (i + 1) & (m->cap - 1)) if (m->e[i].k == k) return &m->e[i];
    return NULL;
}
static void map_put(dbmap *m, dbent v) {
    pthread_mutex_lock(&m->mu);
    if ((m->n + 1) * 2 > m->cap) { dbmap o = *m; m->cap = m->cap ? m->cap * 2 : 1024; m->e = calloc(m->cap, sizeof *m->e); m->n = 0;
        for (uint32_t i = 0; i < o.cap; i++) if (o.e[i].k) { uint32_t j = h64(o.e[i].k, m->cap); while (m->e[j].k) j = (j + 1) & (m->cap - 1); m->e[j] = o.e[i]; m->n++; }
        free(o.e); }
    uint32_t i = h64(v.k, m->cap); while (m->e[i].k && m->e[i].k != v.k) i = (i + 1) & (m->cap - 1);
    if (!m->e[i].k) m->n++; m->e[i] = v;
    pthread_mutex_unlock(&m->mu);
}
static void map_del(dbmap *m, uint64_t k) {   // backward-shift delete
    pthread_mutex_lock(&m->mu);
    dbent *e = map_find(m, k);
    if (e) { uint32_t h = (uint32_t)(e - m->e), j = h;
        for (;;) { j = (j + 1) & (m->cap - 1); if (!m->e[j].k) break; uint32_t w = h64(m->e[j].k, m->cap);
            if (((j - w) & (m->cap - 1)) >= ((j - h) & (m->cap - 1))) { m->e[h] = m->e[j]; h = j; } }
        m->e[h].k = 0; m->n--; }
    pthread_mutex_unlock(&m->mu);
}
static VKAPI_ATTR VkResult VKAPI_CALL w_create_buffer(VkDevice d, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *cb, VkBuffer *b) {
    VkBufferCreateInfo c = *ci; c.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VkResult r = r_create_buffer(d, &c, cb, b);
    if (!r) map_put(&g_bufs, (dbent){(uint64_t)(uintptr_t)*b, c.size, 0, 0, 0});
    return r;
}
static _Atomic uint64_t g_kill_gen;   // a buffer or view destroyed: the threads' caches below start again (its handle may come back for another)
static VKAPI_ATTR void VKAPI_CALL w_destroy_buffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks *cb) { if (b) { map_del(&g_bufs, (uint64_t)(uintptr_t)b); g_kill_gen++; } r_destroy_buffer(d, b, cb); }
static VKAPI_ATTR VkResult VKAPI_CALL w_alloc(VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *cb, VkDeviceMemory *m) {
    for (const VkBaseInStructure *p = ai->pNext; p; p = p->pNext) if (p->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO) return r_alloc(d, ai, cb, m);
    VkMemoryAllocateFlagsInfo fi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, ai->pNext, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
    VkMemoryAllocateInfo a = *ai; a.pNext = &fi;
    return r_alloc(d, &a, cb, m);
}
static VKAPI_ATTR VkResult VKAPI_CALL w_create_bview(VkDevice d, const VkBufferViewCreateInfo *ci, const VkAllocationCallbacks *cb, VkBufferView *v) {
    VkResult r = r_create_bview(d, ci, cb, v);
    if (!r) map_put(&g_views, (dbent){(uint64_t)(uintptr_t)*v, (uint64_t)(uintptr_t)ci->buffer, ci->offset, ci->range, (uint64_t)ci->format});
    return r;
}
static VKAPI_ATTR void VKAPI_CALL w_destroy_bview(VkDevice d, VkBufferView v, const VkAllocationCallbacks *cb) { if (v) { map_del(&g_views, (uint64_t)(uintptr_t)v); g_kill_gen++; } r_destroy_bview(d, v, cb); }
static VkDeviceAddress buf_addr_(VkBuffer b, VkDeviceSize *size);
static VkDeviceAddress buf_addr(VkBuffer b, VkDeviceSize *size) {   // through the thread's cache
    static __thread struct { uint64_t k, gen; VkDeviceAddress a; VkDeviceSize s; } c[512]; uint64_t g = g_kill_gen; unsigned h = h64((uint64_t)(uintptr_t)b, 512);
    if (c[h].k == (uint64_t)(uintptr_t)b && c[h].gen == g) { if (size) *size = c[h].s; return c[h].a; }
    VkDeviceSize s = 0; VkDeviceAddress a = buf_addr_(b, &s);
    if (a) { c[h].k = (uint64_t)(uintptr_t)b; c[h].gen = g; c[h].a = a; c[h].s = s; }
    if (size) *size = s; return a;
}
static VkDeviceAddress buf_addr_(VkBuffer b, VkDeviceSize *size) {   // 0 when the buffer was not made through the wrapper
    pthread_mutex_lock(&g_bufs.mu); dbent *e = map_find(&g_bufs, (uint64_t)(uintptr_t)b); VkDeviceAddress a = 0;
    if (e) { if (!e->b) { VkBufferDeviceAddressInfo i = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, b}; e->b = p_bda(V.dev, &i); } a = e->b; if (size) *size = e->a; }
    pthread_mutex_unlock(&g_bufs.mu); return a;
}

// ---- the ring the descriptors are written into: one buffer, bound once per command buffer; a submission's bytes are free once the GPU has passed it ----
static VkBuffer g_ring; static uint8_t *g_ring_p; static VkDeviceAddress g_ring_a; static uint64_t g_ring_size;
static uint64_t g_head, g_tail;   // virtual offsets (they only grow): written up to head, still in use from tail
uint64_t vk_db_head(void) { return g_head; }
void vk_db_release(uint64_t upto) { if (upto > g_tail) g_tail = upto; }   // a recycled slot: everything it wrote is free (gfx_vk_core.c slot_recycle)
static uint64_t ring_alloc(uint64_t n) {
    uint64_t al = g_dbp.descriptorBufferOffsetAlignment ? g_dbp.descriptorBufferOffsetAlignment : 64;
    uint64_t at = (g_head + al - 1) & ~(al - 1);
    if (at % g_ring_size + n > g_ring_size) at = (at / g_ring_size + 1) * g_ring_size;   // contiguous: past the end, from the start
    if (at + n - g_tail > g_ring_size) {   // full: wait for everything submitted before this submission (its own writes stay)
        void vk_db_wait_submitted(void); static int said; if (!said++) VKLOG("[RED] descriptor buffer ring full (%llu MB): waiting for the GPU", (unsigned long long)(g_ring_size >> 20));
        vk_db_wait_submitted();
        if (at + n - g_tail > g_ring_size) die("[vk] descriptor buffer ring: one submission's descriptors fill it (%llu MB)", (unsigned long long)(g_ring_size >> 20));
    }
    g_head = at + n; return at % g_ring_size;
}

// ---- layouts: their size and each binding's offset, cached ----
typedef struct { VkDescriptorSetLayout l; VkDeviceSize size; uint32_t nb; uint32_t bind[96]; VkDeviceSize off[96]; } dblay;
static dblay **g_lay; static int g_nlay, g_caplay; static pthread_mutex_t g_lay_mu = PTHREAD_MUTEX_INITIALIZER;   // (each its own allocation: lay_of keeps a pointer)
void vk_db_layout(VkDescriptorSetLayout l, const VkDescriptorSetLayoutBinding *b, uint32_t n) {   // (gfx_vk_core.c vk_dsl_register)
    if (!g_db_on) return;
    dblay *d = calloc(1, sizeof *d); d->l = l; p_layout_size(V.dev, l, &d->size);
    for (uint32_t i = 0; i < n && i < 96; i++) { d->bind[d->nb] = b[i].binding; p_binding_off(V.dev, l, b[i].binding, &d->off[d->nb]); d->nb++; }
    if (d->size > g_dbp.maxSamplerDescriptorBufferRange) VKLOG("[RED] descriptor buffer: a set layout of %llu bytes, past the sampler range", (unsigned long long)d->size);
    pthread_mutex_lock(&g_lay_mu);
    if (g_nlay == g_caplay) { g_caplay = g_caplay ? 2 * g_caplay : 256; g_lay = realloc(g_lay, sizeof *g_lay * (size_t)g_caplay); }
    g_lay[g_nlay++] = d; pthread_mutex_unlock(&g_lay_mu);
}
static const dblay *lay_of(VkDescriptorSetLayout l) {   // a thread's table first: the scan under the lock was the replay thread's busiest place (each stage has its own layout)
    static __thread const dblay *tab[256]; unsigned h = h64((uint64_t)(uintptr_t)l, 256);
    if (tab[h] && tab[h]->l == l) return tab[h];
    pthread_mutex_lock(&g_lay_mu); const dblay *r = NULL; for (int i = g_nlay - 1; i >= 0; i--) if (g_lay[i]->l == l) { r = g_lay[i]; break; } pthread_mutex_unlock(&g_lay_mu);
    if (!r) die("[vk] descriptor buffer: layout %p was not registered", (void *)l);
    return tab[h] = r;   // (layouts are never destroyed)
}
static size_t desc_size(VkDescriptorType t) {
    switch (t) {
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return g_dbp.uniformBufferDescriptorSize;
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return g_dbp.storageBufferDescriptorSize;
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return g_dbp.combinedImageSamplerDescriptorSize;
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: return g_dbp.storageImageDescriptorSize;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: return g_dbp.uniformTexelBufferDescriptorSize;
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return g_dbp.storageTexelBufferDescriptorSize;
    default: return 0;
    }
}
// A "set": its offset in the ring plus one (low 32 bits) and its size (high 32)
#define SET_ENC(off, size) ((uint64_t)(off) + 1 + ((uint64_t)(size) << 32))
#define SET_OFF(s) (((uint64_t)(uintptr_t)(s) & 0xffffffffull) - 1)
#define SET_SIZE(s) ((uint64_t)(uintptr_t)(s) >> 32)
// A set's descriptors written into the ring
VkDescriptorSet vk_db_write(VkDescriptorSetLayout l, const VkWriteDescriptorSet *w, uint32_t nw) {
    const dblay *d = lay_of(l); uint64_t at = ring_alloc(d->size ? d->size : 1); uint8_t *base = g_ring_p + at;
    for (uint32_t i = 0; i < nw; i++) {
        const VkWriteDescriptorSet *x = &w[i]; VkDeviceSize bo = 0; uint32_t j = 0; while (j < d->nb && d->bind[j] != x->dstBinding) j++;
        if (j == d->nb) { VKRED_ONCE(120, "descriptor buffer: binding %u is not in its layout", x->dstBinding); continue; }
        bo = d->off[j]; size_t ds = desc_size(x->descriptorType);
        if (!ds) { VKRED_ONCE(121, "descriptor buffer: descriptor type %d not handled", x->descriptorType); continue; }
        for (uint32_t k = 0; k < x->descriptorCount; k++) {
            VkDescriptorGetInfoEXT gi = {VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT, NULL, x->descriptorType, {0}}; VkDescriptorAddressInfoEXT ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
            switch (x->descriptorType) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
                const VkDescriptorBufferInfo *b = &x->pBufferInfo[k]; VkDeviceSize sz = 0; VkDeviceAddress a = b->buffer ? buf_addr(b->buffer, &sz) : 0;
                if (a) { ai.address = a + b->offset; ai.range = b->range == VK_WHOLE_SIZE ? sz - b->offset : b->range; ai.format = VK_FORMAT_UNDEFINED; }
                else if (b->buffer) VKRED_ONCE(122, "descriptor buffer: a buffer with no device address (made before the wrapper?)");
                if (x->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) gi.data.pUniformBuffer = a ? &ai : NULL; else gi.data.pStorageBuffer = a ? &ai : NULL;
                break; }
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
                VkBufferView v = x->pTexelBufferView[k]; int ok = 0;
                if (v) { static __thread struct { dbent e; uint64_t gen; } vc[512]; uint64_t g = g_kill_gen; unsigned h = h64((uint64_t)(uintptr_t)v, 512); dbent c;
                    if (vc[h].e.k == (uint64_t)(uintptr_t)v && vc[h].gen == g) c = vc[h].e;
                    else { pthread_mutex_lock(&g_views.mu); dbent *e = map_find(&g_views, (uint64_t)(uintptr_t)v); c = e ? *e : (dbent){0}; pthread_mutex_unlock(&g_views.mu); if (c.k) { vc[h].e = c; vc[h].gen = g; } }
                    VkDeviceSize sz = 0; VkDeviceAddress a = c.k ? buf_addr((VkBuffer)(uintptr_t)c.a, &sz) : 0;
                    if (a) { ai.address = a + c.b; ai.range = c.c == VK_WHOLE_SIZE ? sz - c.b : c.c; ai.format = (VkFormat)c.d; ok = 1; } }
                if (x->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) gi.data.pUniformTexelBuffer = ok ? &ai : NULL; else gi.data.pStorageTexelBuffer = ok ? &ai : NULL;
                break; }
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: gi.data.pCombinedImageSampler = &x->pImageInfo[k]; break;
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: gi.data.pStorageImage = &x->pImageInfo[k]; break;
            default: break;
            }
            p_get(V.dev, &gi, ds, base + bo + ds * (x->dstArrayElement + k));
        }
    }
    return (VkDescriptorSet)(uintptr_t)(SET_ENC(at, d->size ? d->size : 1));
}

// The sets of a draw or dispatch bound. One binding of a buffer with sampler descriptors reaches maxSamplerDescriptorBufferRange bytes (128 KB on Adreno 825),
// so the ring is bound as a window that moves with what is written: the sets of one call must lie in the window; a set outside it (a set the submission's cache
// gave back from earlier) is copied to the head, and when the head leaves the window the ring is bound again from it. Binding again makes every offset set
// so far point elsewhere: the draws' bind cache is told (g_bc_stale) and a dispatch sets its own each time
static VkCommandBuffer g_bound_cb; static uint64_t g_bound_gen, g_win = ~0ull;   // the window: its start in the ring
void vk_db_bind(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t first, uint32_t n, const VkDescriptorSet *sets) {
    extern uint64_t g_cmd_gen; extern int g_bc_stale; uint64_t range = g_dbp.maxSamplerDescriptorBufferRange, al = g_dbp.descriptorBufferOffsetAlignment ? g_dbp.descriptorBufferOffsetAlignment : 64;
    VkDescriptorSet s[5]; for (uint32_t i = 0; i < n && i < 5; i++) s[i] = sets[i];
    int fresh = cb != g_bound_cb || g_bound_gen != g_cmd_gen;
    for (int pass = 0; pass < 3; pass++) {   // the sets in the window; else a window from the head, with the sets copied there (again if the ring wrapped meanwhile)
        int in = !fresh && g_win != ~0ull;
        for (uint32_t i = 0; i < n && in; i++) { uint64_t o = SET_OFF(s[i]), z = SET_SIZE(s[i]); if (o < g_win || o + z > g_win + range) in = 0; }
        if (in) break;
        if (pass == 2) die("[vk] descriptor buffer: a call's sets do not fit one window");
        uint64_t lo = ~0ull, hi = 0; int near = 1;
        for (uint32_t i = 0; i < n; i++) { uint64_t o = SET_OFF(s[i]), z = SET_SIZE(s[i]); if (o < lo) lo = o; if (o + z > hi) hi = o + z; }
        if (n && hi - (lo & ~(al - 1)) > range) near = 0;
        if (pass) near = 0;
        if (!near) {   // spread wider than a window: each one copied to the head, together
            for (uint32_t i = 0; i < n; i++) { uint64_t z = SET_SIZE(s[i]), o = ring_alloc(z); memcpy(g_ring_p + o, g_ring_p + SET_OFF(s[i]), (size_t)z); s[i] = (VkDescriptorSet)(uintptr_t)SET_ENC(o, z); }
            lo = ~0ull; for (uint32_t i = 0; i < n; i++) if (SET_OFF(s[i]) < lo) lo = SET_OFF(s[i]);
        }
        g_win = n ? lo & ~(al - 1) : 0; if (g_win + range > g_ring_size) g_win = g_ring_size > range ? (g_ring_size - range) & ~(al - 1) : 0;
        VkDescriptorBufferBindingInfoEXT bi = {VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT, NULL, g_ring_a + g_win,
            VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT};
        vkCmdBindDescriptorBuffersEXT(cb, 1, &bi); g_bound_cb = cb; g_bound_gen = g_cmd_gen; g_bc_stale = 1; fresh = 0;
        static uint64_t nb; if (++nb % 10000 == 0) VKLOG("descriptor buffer: %llu window binds", (unsigned long long)nb);
    }
    uint32_t idx[5] = {0}; VkDeviceSize off[5];
    for (uint32_t i = 0; i < n && i < 5; i++) off[i] = SET_OFF(s[i]) - g_win;
    vkCmdSetDescriptorBufferOffsetsEXT(cb, bp, layout, first, n, idx, off);
}

// Device creation: the extension and its feature, when asked for and the device has them (gfx_vk_core.c vk_init)
int vk_db_wanted(void) { const char *e = getenv("DM_VK_DESCBUF"); return e && atoi(e); }
// After the device and its functions: the properties, the wrappers, the ring
void vk_db_init(int enabled) {
    if (!enabled) return;
    #define DBL(var, name) var = (void *)vkGetDeviceProcAddr(V.dev, name)
    DBL(p_layout_size, "vkGetDescriptorSetLayoutSizeEXT"); DBL(p_binding_off, "vkGetDescriptorSetLayoutBindingOffsetEXT"); DBL(p_get, "vkGetDescriptorEXT");
    DBL(vkCmdBindDescriptorBuffersEXT, "vkCmdBindDescriptorBuffersEXT"); DBL(vkCmdSetDescriptorBufferOffsetsEXT, "vkCmdSetDescriptorBufferOffsetsEXT");
    DBL(p_bda, "vkGetBufferDeviceAddress"); if (!p_bda) DBL(p_bda, "vkGetBufferDeviceAddressKHR");
    if (!p_layout_size || !p_binding_off || !p_get || !vkCmdBindDescriptorBuffersEXT || !vkCmdSetDescriptorBufferOffsetsEXT || !p_bda) { VKLOG("[RED] descriptor buffers: functions missing, not used"); return; }
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &g_dbp}; vkGetPhysicalDeviceProperties2(V.pd, &p2);
    VKLOG("descriptor buffers: offset alignment %llu; descriptor sizes: uniform buffer %zu, storage buffer %zu, combined image sampler %zu, storage image %zu, texel buffers %zu / %zu; "
          "ranges: resource %llu KB, sampler %llu KB (address space %llu / %llu KB); bindings %u (resource %u, sampler %u)", (unsigned long long)g_dbp.descriptorBufferOffsetAlignment,
          g_dbp.uniformBufferDescriptorSize, g_dbp.storageBufferDescriptorSize, g_dbp.combinedImageSamplerDescriptorSize, g_dbp.storageImageDescriptorSize,
          g_dbp.uniformTexelBufferDescriptorSize, g_dbp.storageTexelBufferDescriptorSize, (unsigned long long)(g_dbp.maxResourceDescriptorBufferRange >> 10),
          (unsigned long long)(g_dbp.maxSamplerDescriptorBufferRange >> 10), (unsigned long long)(g_dbp.resourceDescriptorBufferAddressSpaceSize >> 10), (unsigned long long)(g_dbp.samplerDescriptorBufferAddressSpaceSize >> 10), g_dbp.maxDescriptorBufferBindings, g_dbp.maxResourceDescriptorBufferBindings, g_dbp.maxSamplerDescriptorBufferBindings);
    r_create_buffer = vkCreateBuffer; vkCreateBuffer = w_create_buffer; r_destroy_buffer = vkDestroyBuffer; vkDestroyBuffer = w_destroy_buffer;
    r_alloc = vkAllocateMemory; vkAllocateMemory = w_alloc; r_create_bview = vkCreateBufferView; vkCreateBufferView = w_create_bview; r_destroy_bview = vkDestroyBufferView; vkDestroyBufferView = w_destroy_bview;
    g_ring_size = 64ull << 20;   // (one binding reaches maxSamplerDescriptorBufferRange of it: vk_db_bind's window)
    if (g_dbp.samplerDescriptorBufferAddressSpaceSize && g_ring_size > g_dbp.samplerDescriptorBufferAddressSpaceSize / 2) g_ring_size = g_dbp.samplerDescriptorBufferAddressSpaceSize / 2;
    if (!g_dbp.maxSamplerDescriptorBufferRange) { VKLOG("[RED] descriptor buffers: no sampler range, not used"); return; }
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, g_ring_size, VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT, VK_SHARING_MODE_EXCLUSIVE};
    VKCHK(vkCreateBuffer(V.dev, &bi, NULL, &g_ring));
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(V.dev, g_ring, &rq);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rq.size, vk_mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
    VkDeviceMemory m; VKCHK(vkAllocateMemory(V.dev, &ai, NULL, &m)); VKCHK(vkBindBufferMemory(V.dev, g_ring, m, 0));
    void *p; VKCHK(vkMapMemory(V.dev, m, 0, VK_WHOLE_SIZE, 0, &p)); g_ring_p = p; g_ring_a = buf_addr(g_ring, NULL);
    g_db_on = 1;
    VKLOG("descriptor buffers in use (DM_VK_DESCBUF): a ring of %llu MB", (unsigned long long)(g_ring_size >> 20));
}
