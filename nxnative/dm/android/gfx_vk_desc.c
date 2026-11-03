// Android port, Vulkan backend: descriptor pools and the descriptor set cache (see gfx_vk.h)
#include "gfx_vk.h"
#include "nvn_hmap.h"
#include <string.h>

// Descriptor pools: the allocator counts what each set takes and moves to a new pool before any count runs out. Adreno's driver does not refuse an
// allocation past a pool's sizes: it succeeds and the submission then fails as a whole at vkEndCommandBuffer (VK_ERROR_OUT_OF_HOST_MEMORY; NEO's
// handheld frames with 32-slot texture arrays crossed the old 8192 combined-sampler limit at about the 712th draw)
enum { DP_SETS = 4096 };
static const uint32_t DP_CAP[VK_NDT] = {[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC] = 16384, [VK_DESCRIPTOR_TYPE_SAMPLER] = 1024, [VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER] = 65536, [VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE] = 1024,
    [VK_DESCRIPTOR_TYPE_STORAGE_IMAGE] = 2048, [VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER] = 4096, [VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER] = 2048,
    [VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER] = 32768, [VK_DESCRIPTOR_TYPE_STORAGE_BUFFER] = 4096, [VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC] = 8192};
static nvn_hmap g_dsl_need = {.klen = sizeof(VkDescriptorSetLayout)}; static pthread_mutex_t g_dsl_mu = PTHREAD_MUTEX_INITIALIZER;
void vk_dsl_register(VkDescriptorSetLayout l, const VkDescriptorSetLayoutBinding *b, uint32_t n) {   // what one set of this layout takes, by type
    uint32_t *need = calloc(VK_NDT, sizeof *need);
    for (uint32_t i = 0; i < n; i++) if ((uint32_t)b[i].descriptorType < VK_NDT) need[b[i].descriptorType] += b[i].descriptorCount;
    pthread_mutex_lock(&g_dsl_mu); nvn_hm_put(&g_dsl_need, &l, need); pthread_mutex_unlock(&g_dsl_mu);
}
VkDescriptorSet vk_dp_alloc(vk_dpools *d, VkDescriptorSetLayout l) {
    pthread_mutex_lock(&g_dsl_mu); const uint32_t *need = nvn_hm_get(&g_dsl_need, &l); pthread_mutex_unlock(&g_dsl_mu);
    for (;;) {
        if (d->cur == d->ndp) {
            VkDescriptorPoolSize sz[VK_NDT]; uint32_t nz = 0;
            for (int t = 0; t < VK_NDT; t++) if (DP_CAP[t]) sz[nz++] = (VkDescriptorPoolSize){(VkDescriptorType)t, DP_CAP[t]};
            VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, DP_SETS, nz, sz};
            if (d->ndp == d->capdp) { d->capdp = d->capdp ? 2 * d->capdp : 4; d->dp = realloc(d->dp, d->capdp * sizeof *d->dp); }
            VKCHK(vkCreateDescriptorPool(V.dev, &pi, NULL, &d->dp[d->ndp++]));
        }
        if (d->cur != d->counted) { memset(d->used, 0, sizeof d->used); d->sets = 0; d->counted = d->cur; }
        int fits = d->sets < DP_SETS;
        if (need) for (int t = 0; t < VK_NDT && fits; t++) if (need[t] && d->used[t] + need[t] > DP_CAP[t]) fits = 0;
        if (!fits) { d->cur++; continue; }
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, d->dp[d->cur], 1, &l};
        VkDescriptorSet set; VkResult r = vkAllocateDescriptorSets(V.dev, &ai, &set);
        if (r == VK_SUCCESS) { d->sets++; if (need) for (int t = 0; t < VK_NDT; t++) d->used[t] += need[t]; return set; }
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) die("[vk] vkAllocateDescriptorSets: %d", r);
        d->cur++;
    }
}
// only the pools sets were taken from since the last reset (counted is -1 until one is): through gfxstream each reset is a round trip to the host, and most
// submissions take no set of their own (the set cache's are kept), so resetting every pool of the slot was ~1 ms of Donkey Kong Bananza's frame
void vk_dp_reset(vk_dpools *d) {
    if (d->counted >= 0) for (int i = 0; i <= d->cur && i < d->ndp; i++) vkResetDescriptorPool(V.dev, d->dp[i], 0);
    d->cur = 0; d->counted = -1;
}
static vk_dpools g_kept;   // pools never reset: sets the cache writes again, the empty set

// Descriptor sets are found by their whole content (layout, and every buffer range, view, sampler and texel buffer view written) across submissions: a set
// whose last use the GPU has finished is written again for other content, and a destroyed view or buffer (vk_obj_gen) retires every set, as they may name it.
// Every new set is an allocation and a vkUpdateDescriptorSets. When the cache lived one submission, Donkey Kong Bananza's title menu made a new set for 68 of
// its 66 draws every frame, and its gameplay ~230 of ~1,500 a frame
enum { SETC = 8192, SETW = 4 };   // SETW: the entries a content hash may take, side by side
typedef struct { uint64_t h, fence, gen; VkDescriptorSetLayout l; VkDescriptorSet set; } setc_t;   // fence: the submission of its last use
static setc_t g_setc[SETC];
static nvn_hmap g_setfree = {.klen = sizeof(VkDescriptorSetLayout)};   // layout → kept sets no entry holds and the GPU is done with
typedef struct { VkDescriptorSet *v; int n, cap; } setfree_t;
static uint64_t g_nkept;   // sets taken from the long-lived pools
VkDescriptorSet vk_set_find(uint64_t h, VkDescriptorSetLayout l) {
    setc_t *e = &g_setc[h % SETC & ~(uint64_t)(SETW - 1)]; uint64_t gen = vk_obj_gen();
    for (int i = 0; i < SETW; i++) if (e[i].set && e[i].h == h && e[i].l == l && e[i].gen == gen) { e[i].fence = mtl_last_fence() + 1; return e[i].set; }
    return VK_NULL_HANDLE;
}
// a set to write for this content: an entry's, when one is free or done with (its set goes to its layout's free list); else one of this submission alone
VkDescriptorSet vk_set_new(uint64_t h, VkDescriptorSetLayout l) {
    setc_t *e = &g_setc[h % SETC & ~(uint64_t)(SETW - 1)], *v = NULL; uint64_t done = vk_fence_done();
    for (int i = 0; i < SETW; i++) {   // an empty entry, else the one the GPU finished with longest ago
        if (!e[i].set) { v = &e[i]; break; }
        if (e[i].fence <= done && (!v || e[i].fence < v->fence)) v = &e[i];
    }
    if (!v || g_nkept > 4 * SETC) return vk_alloc_set(l);
    if (v->set) { setfree_t *f = nvn_hm_get(&g_setfree, &v->l); if (!f) { f = calloc(1, sizeof *f); nvn_hm_put(&g_setfree, &v->l, f); }
        if (f->n == f->cap) { f->cap = f->cap ? 2 * f->cap : 16; f->v = realloc(f->v, sizeof *f->v * (size_t)f->cap); } f->v[f->n++] = v->set; }
    setfree_t *f = nvn_hm_get(&g_setfree, &l); VkDescriptorSet set = f && f->n ? f->v[--f->n] : VK_NULL_HANDLE;
    if (!set) { set = vk_dp_alloc(&g_kept, l); g_nkept++; }
    *v = (setc_t){h, mtl_last_fence() + 1, vk_obj_gen(), l, set};
    return set;
}
VkDescriptorSet vk_empty_set(VkDescriptorSetLayout l) {
    static VkDescriptorSetLayout el; static VkDescriptorSet es;
    if (l != el) { el = l; es = vk_dp_alloc(&g_kept, l); }
    return es;
}
