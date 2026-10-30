// Android port, Vulkan backend: the game's shaders (see gfx_vk.h).
// The bundle (dm/android/tools/shader_vk.py) is mapped read-only: entries sorted by key, each the SPIR-V of one Switch shader and its binary reflection.
// The translator numbers every stage's bindings from 0 in set 0; at module creation each stage's bindings move to a set of its own (vertex 0, fragment 1,
// geometry 2, tessellation 3 / 4; compute 0) by rewriting the DescriptorSet decorations, so a program's stages never collide.
#include "gfx_vk.h"
#include "nvn_hmap.h"
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct __attribute__((packed)) { uint64_t key; uint32_t stage, spv_off, spv_len, refl_off, refl_len, pad; } bent;
static const uint8_t *g_bundle; static uint32_t g_nent; static const bent *g_ent;
void vk_shaders_open(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); struct stat st;
    if (fd < 0 || fstat(fd, &st)) { VKLOG("[RED] no shader bundle at %s: nothing will be drawn", path); if (fd >= 0) close(fd); return; }
    g_bundle = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0); close(fd);
    if (g_bundle == MAP_FAILED || memcmp(g_bundle, "DMSV", 4)) { VKLOG("[RED] %s is not a shader bundle", path); g_bundle = NULL; return; }
    memcpy(&g_nent, g_bundle + 8, 4); g_ent = (const bent *)(g_bundle + 12);
    VKLOG("shader bundle %s: %u shaders, %.1f MB", path, g_nent, st.st_size / 1048576.0);
}
static void bundle_open_once(void) {
    const char *p = getenv("DM_VK_SHADERS"); char buf[1024];
    if (!p) { const char *r = getenv("DM_ROOT"); snprintf(buf, sizeof buf, "%s/shaders.dmsv", r ? r : "."); p = buf; }
    vk_shaders_open(p);
}
// once, and every caller waits until it is open: the replay and the prewarm threads both look shaders up at start, and a lookup that ran before the bundle
// was open cached its shader as missing for good (every draw skipped as "no program" in some runs)
static void bundle_auto(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, bundle_open_once); }
static const bent *find(uint64_t key) {
    uint32_t lo = 0, hi = g_nent;
    while (lo < hi) { uint32_t m = (lo + hi) / 2; uint64_t k; memcpy(&k, &g_ent[m].key, 8); if (k < key) lo = m + 1; else hi = m; }
    if (lo < g_nent) { uint64_t k; memcpy(&k, &g_ent[lo].key, 8); if (k == key) return &g_ent[lo]; }
    return NULL;
}
int vk_shader_stage(uint64_t key) { bundle_auto(); const bent *e = g_bundle ? find(key) : NULL; return e ? (int)e->stage : -1; }   // the NVN stage the bundle has this shader as

// Run-time capture: a shader a draw needs that the bundle does not have is saved as DM_ROOT/captured_shaders/<key>.bin ("DMSC", u32 control length, u32 code length, the control
// section as its key hashed it, the code area), and the porter translates the folder into the bundle at the next port (porter/pipeline.py, shader_vk.py --capture).
// Xenoblade Chronicles 3 sets hundreds of programs whose code is in no game file (GLSLC outputs of its Firehawk effects, and vertex / pixel shaders of models: a machine near
// the camera in Everlight Plain drew as a black silhouette, its depth pre-pass drawn and its G-buffer draw skipped); 51 of them in the opening and the first field.
// The game may free a control section once SetShaders returns, so SetShaders keeps a copy of each one the bundle lacks (shader_set); the file is written only when a draw misses
// the key (vk_shader_missing) and the code area then still hashes to it
typedef struct { uint64_t key, data; uint32_t nctl, ncode; uint8_t *ctl; } pend_t;
enum { NPEND = 4096 };
static pend_t g_pend[NPEND]; static unsigned g_npend; static pthread_mutex_t g_pend_mu = PTHREAD_MUTEX_INITIALIZER;
static void shader_set(const uint8_t k[8], const void *control, uint64_t data, uint32_t ncode) {
    uint64_t key = 0; for (int i = 0; i < 8; i++) key = key << 8 | k[i];
    if (!key || !control || !data || !ncode || ncode >= (1u << 24) || vk_shader_stage(key) >= 0) return;
    uint32_t nctl = 0x880, own; memcpy(&own, (const uint8_t *)control + 0x20, 4); if (own >= 0x760 && own < 0x880) nctl = own;   // as hle_nvn.c keys_hash
    uint8_t *c = malloc(nctl); if (!c) return; memcpy(c, control, nctl);
    pthread_mutex_lock(&g_pend_mu);
    pend_t *p = &g_pend[g_npend++ % NPEND]; free(p->ctl); *p = (pend_t){key, data, nctl, ncode, c};   // the oldest goes: a draw needs a program soon after it is set
    pthread_mutex_unlock(&g_pend_mu);
}
__attribute__((constructor)) static void shader_set_on(void) { extern void (*g_nvn_on_shader)(const uint8_t *, const void *, uint64_t, uint32_t); g_nvn_on_shader = shader_set; }
void vk_shader_missing(uint64_t key) {
    pend_t p = {0};
    pthread_mutex_lock(&g_pend_mu);
    for (unsigned i = 0; i < NPEND && i < g_npend; i++) if (g_pend[i].key == key && g_pend[i].ctl) { p = g_pend[i]; p.ctl = malloc(p.nctl); if (p.ctl) memcpy(p.ctl, g_pend[i].ctl, p.nctl); break; }
    pthread_mutex_unlock(&g_pend_mu);
    if (!p.ctl) { VKLOG("[RED] shader %016llx is not in the bundle (and was not set while the capture could see it)", (unsigned long long)key); return; }
    uint8_t md[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_CTX hc; CC_SHA256_Init(&hc); CC_SHA256_Update(&hc, p.ctl, p.nctl); CC_SHA256_Update(&hc, (const void *)(uintptr_t)p.data, p.ncode); CC_SHA256_Final(md, &hc);
    uint64_t now = 0; for (int i = 0; i < 8; i++) now = now << 8 | md[i];
    if (now != key) { VKLOG("[RED] shader %016llx is not in the bundle; its code area changed since SetShaders, not saved", (unsigned long long)key); free(p.ctl); return; }
    const char *r = getenv("DM_ROOT"); char dir[1024], path[1100], tmp[1110]; snprintf(dir, sizeof dir, "%s/captured_shaders", r ? r : "."); mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/%016llx.bin", dir, (unsigned long long)key); snprintf(tmp, sizeof tmp, "%s.part", path);
    struct stat st; if (!stat(path, &st)) { VKLOG("[RED] shader %016llx is not in the bundle: captured in %s by an earlier run, the next port translates it", (unsigned long long)key, path); free(p.ctl); return; }
    FILE *f = fopen(tmp, "wb"); uint32_t hdr[3] = {0x43534d44, p.nctl, p.ncode};   // "DMSC"
    int ok = f && fwrite(hdr, 1, 12, f) == 12 && fwrite(p.ctl, 1, p.nctl, f) == p.nctl && fwrite((const void *)(uintptr_t)p.data, 1, p.ncode, f) == p.ncode;
    if (f && fclose(f)) ok = 0;
    if (!ok || rename(tmp, path)) { VKLOG("[RED] shader %016llx is not in the bundle, and %s could not be written", (unsigned long long)key, path); unlink(tmp); }
    else VKLOG("[RED] shader %016llx is not in the bundle: saved to %s, the next port translates it", (unsigned long long)key, path);
    free(p.ctl);
}
static VkShaderStageFlags stage_flag(int st) {
    switch (st) { case 0: return VK_SHADER_STAGE_VERTEX_BIT; case 1: return VK_SHADER_STAGE_FRAGMENT_BIT; case 2: return VK_SHADER_STAGE_GEOMETRY_BIT;
                  case 3: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT; case 4: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT; default: return VK_SHADER_STAGE_COMPUTE_BIT; }
}
static int set_of(int st) { return st == 5 ? 0 : st == 0 ? 0 : st == 1 ? 1 : st == 2 ? 2 : st; }
static VkDescriptorType desc_type(int d) {
    switch (d) { case RD_UBO: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; case RD_SSBO: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; case RD_SAMPLED: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                 case RD_STORAGE: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; case RD_TBUF: return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER; case RD_STORAGE_TBUF: return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
                 case RD_IMAGE: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; default: return VK_DESCRIPTOR_TYPE_SAMPLER; }
}
// Stage interfaces. Vulkan requires every input location of a stage to be declared as an output of the stage before it (VUID-RuntimeSpirv-OpEntryPoint-08743);
// the translator writes each stage apart, so a stage may read locations the one before never declares. Xenoblade Chronicles 3's passthrough geometry shaders
// (written out as ordinary ones, forwarding all 16 attributes) follow vertex shaders with two outputs, and Adreno refused those pipelines (VK_ERROR_UNKNOWN).
// The earlier stage then gets a variant that declares the missing vec4 outputs and leaves them unwritten; a vertex shader's variant then stores (0, 0, 0, 1) to them
// as to its own outputs (spv_vs_init), the values Maxwell gives unwritten outputs, where Vulkan leaves them undefined.
static int spv_is_vec4(const uint32_t *w, uint32_t fn, uint32_t t) {   // a vec4 of 32-bit floats, or an array of them (per-vertex inputs)
    for (int depth = 0; depth < 3; depth++) { uint32_t i = 5, found = 0;
        while (i < fn) { uint32_t n = w[i] >> 16; if (!n) return 0; if (w[i + 1] == t && ((w[i] & 0xffff) == 23 || (w[i] & 0xffff) == 28 || (w[i] & 0xffff) == 32)) { found = i; break; } i += n; }
        if (!found) return 0; uint32_t op = w[found] & 0xffff;
        if (op == 32) { t = w[found + 3]; continue; }   // OpTypePointer
        if (op == 28) { t = w[found + 2]; continue; }   // OpTypeArray
        if (w[found + 3] != 4) return 0; for (i = 5; i < fn;) { if ((w[i] & 0xffff) == 22 && w[i + 1] == w[found + 2]) return w[i + 2] == 32; i += w[i] >> 16; } return 0; }
    return 0;
}
static void spv_io_locs(const uint32_t *w, uint32_t nw, uint32_t *in, uint32_t *out) {
    *in = *out = 0; uint32_t fn = nw;
    for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16; if (!n) return; if ((w[i] & 0xffff) == 54) { fn = i; break; } i += n; }
    for (uint32_t i = 5; i < fn;) { uint32_t n = w[i] >> 16;
        if ((w[i] & 0xffff) == 71 && n == 4 && w[i + 2] == 30 && w[i + 3] < 32) {   // OpDecorate Location
            uint32_t v = w[i + 1];
            for (uint32_t j = 5; j < fn;) { uint32_t m = w[j] >> 16; if ((w[j] & 0xffff) == 59 && w[j + 2] == v) {
                    if (spv_is_vec4(w, fn, w[j + 1])) { if (w[j + 3] == 1) *in |= 1u << w[i + 3]; else if (w[j + 3] == 3) *out |= 1u << w[i + 3]; }
                    else if (w[j + 3] == 3) *out |= 1u << w[i + 3];   // an output of another type still takes its location
                    break; }
                j += m; } }
        i += n; }
}
static int spv_num_kind(const uint32_t *w, uint32_t fn, uint32_t t) {   // the numeric type under a pointer / array / vector type: 1 uint, 2 int, 3 float (32-bit), 0 other
    for (int depth = 0; depth < 4; depth++) { uint32_t i = 5, at = 0;
        while (i < fn) { uint32_t n = w[i] >> 16; if (!n) return 0; if (w[i + 1] == t && (w[i] & 0xffff) >= 19 && (w[i] & 0xffff) <= 39) { at = i; break; } i += n; }
        if (!at) return 0; uint32_t op = w[at] & 0xffff;
        if (op == 32) { t = w[at + 3]; continue; }                       // OpTypePointer
        if (op == 23 || op == 28) { t = w[at + 2]; continue; }           // OpTypeVector, OpTypeArray
        if (op == 21) return w[at + 2] == 32 ? (w[at + 3] ? 2 : 1) : 0;  // OpTypeInt width signedness
        if (op == 22) return w[at + 2] == 32 ? 3 : 0;                    // OpTypeFloat
        return 0; }
    return 0;
}
static void spv_in_kinds(const uint32_t *w, uint32_t nw, uint8_t kind[32]) {   // vkshader_t in_kind: each input location's numeric type as declared
    memset(kind, 0, 32); uint32_t fn = nw;
    for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16; if (!n) return; if ((w[i] & 0xffff) == 54) { fn = i; break; } i += n; }
    for (uint32_t i = 5; i < fn;) { uint32_t n = w[i] >> 16;
        if ((w[i] & 0xffff) == 71 && n == 4 && w[i + 2] == 30 && w[i + 3] < 32)   // OpDecorate Location
            for (uint32_t j = 5; j < fn;) { uint32_t m = w[j] >> 16; if ((w[j] & 0xffff) == 59 && w[j + 2] == w[i + 1]) { if (w[j + 3] == 1) kind[w[i + 3]] = (uint8_t)spv_num_kind(w, fn, w[j + 1]); break; } j += m; }
        i += n; }
}
// A vertex shader's outputs start as (0, 0, 0, 1), as on Maxwell: the translator stores only the components the shader writes and Vulkan leaves the rest
// undefined (dmsh_build.py vs_init does the same for the Apple chain, found on Link's Awakening's lighting). Pokémon Legends: Arceus's deferred lighting draws
// with vertex shader 1327af7c2b78070e, which writes out0.xy only, and its pixel shaders divide by in0.w: every lit pixel was wrong (a washed-out picture,
// dark hair and cloth turned white). Each vec4 output with a Location gets that value stored at the entry point's start. NULL when the module is not as expected
static uint32_t *spv_vs_init(const uint32_t *w, uint32_t nw, uint32_t *onw) {
    uint32_t bound = w[3], fn = 0, ep = 0, f32 = 0, v4 = 0, c0 = 0, c1 = 0; uint8_t *ptr = calloc(bound, 1), *loc = calloc(bound, 1); uint32_t vars[32], nv = 0;
    for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n || i + n > nw) goto fail;
        if (op == 54) { fn = i; break; }
        if (op == 15 && n >= 3 && w[i + 1] == 0) ep = w[i + 2];   // OpEntryPoint Vertex
        if (op == 22 && n == 3 && w[i + 2] == 32) f32 = w[i + 1];
        if (op == 23 && n == 4 && w[i + 2] == f32 && w[i + 3] == 4 && f32) v4 = w[i + 1];
        if (op == 32 && n == 4 && w[i + 2] == 3 && v4 && w[i + 3] == v4 && w[i + 1] < bound) ptr[w[i + 1]] = 1;   // OpTypePointer Output vec4
        if (op == 43 && n == 4 && w[i + 1] == f32 && f32) { if (w[i + 3] == 0) c0 = w[i + 2]; else if (w[i + 3] == 0x3f800000) c1 = w[i + 2]; }
        if (op == 71 && n == 4 && w[i + 2] == 30 && w[i + 1] < bound) loc[w[i + 1]] = 1;   // Location
        i += n; }
    if (!fn || !ep || !v4) goto fail;
    for (uint32_t i = 5; i < fn;) { uint32_t n = w[i] >> 16; if ((w[i] & 0xffff) == 59 && n >= 4 && w[i + 3] == 3 && w[i + 1] < bound && ptr[w[i + 1]] && w[i + 2] < bound && loc[w[i + 2]] && nv < 32) vars[nv++] = w[i + 2]; i += n; }
    if (!nv) goto fail;
    uint32_t at = 0;   // the entry point's first block, after its OpVariables
    for (uint32_t i = fn; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n) goto fail;
        if (op == 54 && w[i + 2] == ep) { uint32_t j = i + n; while (j < nw && (w[j] & 0xffff) != 248) j += w[j] >> 16; j += 2;
            while (j < nw && ((w[j] & 0xffff) == 59 || (w[j] & 0xffff) == 8 || (w[j] & 0xffff) == 317)) j += w[j] >> 16; at = j; break; }
        i += n; }
    if (!at || at >= nw) goto fail;
    uint32_t dec[16], nd = 0, id = bound;
    if (!c0) { c0 = id++; dec[nd++] = 4u << 16 | 43; dec[nd++] = f32; dec[nd++] = c0; dec[nd++] = 0; }
    if (!c1) { c1 = id++; dec[nd++] = 4u << 16 | 43; dec[nd++] = f32; dec[nd++] = c1; dec[nd++] = 0x3f800000; }
    uint32_t cv = id++; dec[nd++] = 7u << 16 | 44; dec[nd++] = v4; dec[nd++] = cv; dec[nd++] = c0; dec[nd++] = c0; dec[nd++] = c0; dec[nd++] = c1;   // OpConstantComposite
    uint32_t *o = malloc(((size_t)nw + nd + 3 * nv) * 4), no = 0;
    memcpy(o, w, (size_t)fn * 4); no = fn; o[3] = id;
    memcpy(&o[no], dec, nd * 4); no += nd;
    memcpy(&o[no], &w[fn], (size_t)(at - fn) * 4); no += at - fn;
    for (uint32_t k = 0; k < nv; k++) { o[no++] = 3u << 16 | 62; o[no++] = vars[k]; o[no++] = cv; }   // OpStore
    memcpy(&o[no], &w[at], (size_t)(nw - at) * 4); no += nw - at;
    free(ptr); free(loc); *onw = no; return o;
fail:
    free(ptr); free(loc); return NULL;
}
static uint32_t *spv_add_outputs(const uint32_t *w, uint32_t nw, uint32_t mask, uint32_t *onw) {
    uint32_t fn = nw, ep = 0, lastdec = 0, bound = w[3], f32 = 0, v4 = 0, pv4 = 0;
    for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n) return NULL; if (op == 54) { fn = i; break; }
        if (op == 15 && !ep) ep = i; if (op == 71 || op == 72) lastdec = i + n;
        if (op == 22 && w[i + 2] == 32) f32 = w[i + 1];
        i += n; }
    if (!ep || !lastdec || !f32) return NULL;
    for (uint32_t i = 5; i < fn;) { if ((w[i] & 0xffff) == 23 && w[i + 2] == f32 && w[i + 3] == 4) v4 = w[i + 1]; i += w[i] >> 16; }
    if (v4) for (uint32_t i = 5; i < fn;) { if ((w[i] & 0xffff) == 32 && w[i + 2] == 3 && w[i + 3] == v4) pv4 = w[i + 1]; i += w[i] >> 16; }
    int k = __builtin_popcount(mask); uint32_t ids[32], nid = 0;
    uint32_t *o = malloc(((size_t)nw + 16 * k + 16) * 4), no = 5; memcpy(o, w, 20);
    for (int l = 0; l < 32; l++) if (mask >> l & 1) ids[nid++] = bound++;
    uint32_t tv4 = v4 ? v4 : bound++, tpv4 = pv4 ? pv4 : bound++;
    for (uint32_t i = 5; i < fn;) { uint32_t n = w[i] >> 16;
        if (i == ep) { o[no++] = (n + nid) << 16 | 15; memcpy(&o[no], &w[i + 1], (n - 1) * 4); no += n - 1; memcpy(&o[no], ids, nid * 4); no += nid; }   // the entry point lists them
        else { memcpy(&o[no], &w[i], n * 4); no += n; }
        i += n;
        if (i == lastdec) { uint32_t q = 0; for (int l = 0; l < 32; l++) if (mask >> l & 1) { o[no++] = 4u << 16 | 71; o[no++] = ids[q++]; o[no++] = 30; o[no++] = (uint32_t)l; } }   // Location
    }
    if (!v4) { o[no++] = 4u << 16 | 23; o[no++] = tv4; o[no++] = f32; o[no++] = 4; }
    if (!pv4) { o[no++] = 4u << 16 | 32; o[no++] = tpv4; o[no++] = 3; o[no++] = tv4; }
    for (uint32_t q = 0; q < nid; q++) { o[no++] = 4u << 16 | 59; o[no++] = tpv4; o[no++] = ids[q]; o[no++] = 3; }   // OpVariable Output
    memcpy(&o[no], &w[fn], (nw - fn) * 4); no += nw - fn;
    o[3] = bound; *onw = no; return o;
}
static nvn_hmap g_shaders = {.klen = 16};   // {key, stage} → vkshader_t*
static pthread_mutex_t g_sh_mu = PTHREAD_MUTEX_INITIALIZER;
// Shared layouts keep pipeline layouts and descriptor bindings reusable across shaders; g_sh_mu protects this cache too.
static VkDescriptorSetLayout shader_layout(const VkDescriptorSetLayoutBinding *b, uint32_t n) {
    struct { uint32_t n, b[96][4]; } k = {.n = n};
    static nvn_hmap cache = {.klen = sizeof k};
    for (uint32_t i = 0; i < n; i++) {
        uint32_t j = i;
        while (j && k.b[j - 1][0] > b[i].binding) { memcpy(k.b[j], k.b[j - 1], sizeof k.b[j]); j--; }
        k.b[j][0] = b[i].binding; k.b[j][1] = b[i].descriptorType; k.b[j][2] = b[i].descriptorCount; k.b[j][3] = b[i].stageFlags;
    }
    VkDescriptorSetLayout layout = nvn_hm_get(&cache, &k);
    if (!layout) {
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, n, b};
        VKCHK(vkCreateDescriptorSetLayout(V.dev, &li, NULL, &layout));
        void vk_dsl_register(VkDescriptorSetLayout l, const VkDescriptorSetLayoutBinding *b, uint32_t n); vk_dsl_register(layout, b, n);
        nvn_hm_put(&cache, &k, layout);
    }
    return layout;
}
vkshader_t *vk_shader(uint64_t key, int stage) {
    bundle_auto();
    struct { uint64_t key; uint64_t stage; } hk = {key, (uint64_t)stage};
    pthread_mutex_lock(&g_sh_mu);
    vkshader_t *s = nvn_hm_get(&g_shaders, &hk);
    if (s) { pthread_mutex_unlock(&g_sh_mu); return s == (void *)1 ? NULL : s; }
    const bent *e = g_bundle ? find(key) : NULL;
    if (!e) { nvn_hm_put(&g_shaders, &hk, (void *)1); pthread_mutex_unlock(&g_sh_mu);
              static int n; if (n++ < 16) VKLOG("[RED] shader %016llx (stage %d) is not in the bundle", (unsigned long long)key, stage); return NULL; }
    s = calloc(1, sizeof *s); s->key = key; s->stage = stage; s->set = set_of(stage);
    // reflection
    const uint8_t *r = g_bundle + e->refl_off; uint32_t hdr[5]; memcpy(hdr, r, 20);
    s->nbind = (int)hdr[0]; s->c1_len = hdr[1]; memcpy(s->local, hdr + 2, 12);
    s->b = (const dmsv_bind *)(r + 20); s->c1 = r + 20 + 20 * s->nbind;
    // SPIR-V with the stage's set
    uint32_t nw = e->spv_len / 4; uint32_t *w = malloc(e->spv_len); memcpy(w, g_bundle + e->spv_off, e->spv_len);
    int f64_used = 0; uint32_t xb = 4;
    for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n) break; if (op == 71 && n >= 4 && w[i + 2] == 34) w[i + 3] = (uint32_t)s->set;   // OpDecorate DescriptorSet
        if (op == 22 && n == 3 && w[i + 2] == 64) f64_used = 1;   // OpTypeFloat 64
        if (op == 16 && n >= 3 && w[i + 2] == 11) s->xfb = 1;     // OpExecutionMode Xfb: a vertex shader that captures transform feedback (shader_vk.py xfb_layout)
        if (op == 16 && n >= 3 && (w[i + 2] == 12 || w[i + 2] == 14 || w[i + 2] == 15)) s->depth_repl = 1;   // OpExecutionMode DepthReplacing / DepthGreater / DepthLess
        // each buffer's stride: the translator decorates an output XfbBuffer b, then XfbStride s (OpDecorate, or OpMemberDecorate for gl_Position's block)
        { uint32_t d = op == 71 && n >= 4 ? w[i + 2] : op == 72 && n >= 5 ? w[i + 3] : 0, v = op == 71 ? w[i + 3] : op == 72 && n >= 5 ? w[i + 4] : 0;
          if (d == 36) xb = v; else if (d == 37 && xb < 4) s->xfb_stride[xb] = v; }
        i += n; }
    // without VK_EXT_transform_feedback the capture declarations would make the module invalid: they are dropped (the capture's draws then write nothing, as before)
    if (s->xfb && !V.has_xfb) { uint32_t o = 5;
        for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n || i + n > nw) { while (i < nw) w[o++] = w[i++]; break; }
            int drop = (op == 17 && n == 2 && w[i + 1] == 53) || (op == 16 && n >= 3 && w[i + 2] == 11) ||   // OpCapability TransformFeedback, OpExecutionMode Xfb
                       (op == 71 && n >= 3 && (w[i + 2] == 36 || w[i + 2] == 37)) || (op == 72 && n >= 4 && (w[i + 3] == 36 || w[i + 3] == 37));   // XfbBuffer / XfbStride
            if (drop) i += n; else for (uint32_t k = 0; k < n; k++) w[o++] = w[i++]; }
        nw = o; s->xfb = 0; VKRED_ONCE(90, "the device has no transform feedback: shaders that capture it run without"); }
    // the translator declares Float64 in every shader; the device has shaderFloat64 off (Adreno has none), which makes the declaration invalid: when the
    // shader has no 64-bit float type it becomes a second Shader declaration
    if (!f64_used) for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n || op != 17) break; if (n == 2 && w[i + 1] == 10) w[i + 1] = 1; i += n; }
    // the translator declares fragment shader interlock in every pixel shader; the device has no VK_EXT_fragment_shader_interlock: when the shader uses no
    // interlock instruction, its OpCapability and OpExtension are dropped
    { int used = 0; for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n) break; if (op == 5364 || op == 5365) used = 1; i += n; }   // OpBegin/EndInvocationInterlockEXT
      if (!used) { uint32_t o = 5;
        for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n || i + n > nw) { while (i < nw) w[o++] = w[i++]; break; }
            int drop = (op == 17 && n == 2 && (w[i + 1] == 5378 || w[i + 1] == 5363 || w[i + 1] == 5372)) ||   // FragmentShader{Pixel,Sample,ShadingRate}InterlockEXT
                       (op == 10 && !memcmp(&w[i + 1], "SPV_EXT_fragment_shader_interlock", 33));
            if (drop) i += n; else for (uint32_t k = 0; k < n; k++) w[o++] = w[i++]; }
        nw = o; } }
    if (stage == 0) { uint32_t n2 = 0, *w2 = spv_vs_init(w, nw, &n2); if (w2) { free(w); w = w2; nw = n2; } }
    spv_io_locs(w, nw, &s->in_locs, &s->out_locs);
    if (stage == 0) spv_in_kinds(w, nw, s->in_kind);
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, (size_t)nw * 4, w};
    VkResult vr = vkCreateShaderModule(V.dev, &ci, NULL, &s->mod); s->spv = w; s->nspv = nw;
    if (vr) { VKLOG("[RED] shader %016llx: vkCreateShaderModule %d", (unsigned long long)key, vr); free(w); free(s); nvn_hm_put(&g_shaders, &hk, (void *)1); pthread_mutex_unlock(&g_sh_mu); return NULL; }
    // descriptor set layout: one entry per binding
    // The game's constant buffers and the driver segment are dynamic uniform buffers: their offset is given when the set is bound, so a set matches any draw's
    // with the same buffers (Unity updates a constant buffer before nearly every draw, and each update is a shadow at a new offset: NEO made a new set for
    // 94% of its draws, 5 ms a frame). Up to half the device's limit per stage, as the vertex and fragment sets share a pipeline layout. DM_VK_DYN_UBO=0: none.
    static int dyn_on = -1; if (dyn_on < 0) { const char *e = getenv("DM_VK_DYN_UBO"); dyn_on = !(e && !atoi(e)); }
    uint32_t dynmax = dyn_on ? V.props.limits.maxDescriptorSetUniformBuffersDynamic / 2 : 0;
    VkDescriptorSetLayoutBinding lb[96]; uint32_t nl = 0;
    for (int i = 0; i < s->nbind && nl < 96; i++) {
        const dmsv_bind *b = &s->b[i];
        int dyn = b->desc == RD_UBO && (b->kind == RK_CBUF || b->kind == RK_DRV) && b->count == 1 && (uint32_t)s->ndyn < dynmax;
        s->dyn[i] = (uint8_t)dyn; s->ndyn += dyn;
        lb[nl++] = (VkDescriptorSetLayoutBinding){b->binding, dyn ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : desc_type(b->desc), b->count ? b->count : 32, stage_flag(stage), NULL};
    }
    s->dsl = shader_layout(lb, nl);
    nvn_hm_put(&g_shaders, &hk, s);
    pthread_mutex_unlock(&g_sh_mu);
    return s;
}
// Integer texels. Maxwell's texture instructions return the texel's 32-bit words as they are, and the shader decides what they mean; the translator declares
// every sampled image as float, which matches only float views. A view of an integer format, or the stencil of a depth-stencil texture (DepthStencilMode
// STENCIL), must be read through a uint / int image in Vulkan, and Adreno converts the value otherwise. Xenoblade Chronicles 3 tags each pixel's material
// for its lighting from the stencil's bit 3 (shader 094f4afb9c876459 tests the raw word & 8): read as float, every tag was 0 and the lit scene was black.
// A variant of the module retypes those bindings' images to uint / int and bitcasts each texel read back to float, which gives the shader the raw words.
typedef struct { const uint32_t *w; uint32_t fn, bound, *at, *nt, nnt; struct { uint32_t old, kind, id; } map[64]; int nmap; } spvi;
static uint32_t spvi_find(const uint32_t *a, uint32_t lo, uint32_t hi, uint32_t op, const uint32_t *ops, uint32_t nops) {   // a declaration op with these operands (after the result id)
    for (uint32_t j = lo; j < hi;) { uint32_t m = a[j] >> 16; if (!m) break; if ((a[j] & 0xffff) == op && m == nops + 2 && !memcmp(&a[j + 2], ops, nops * 4)) return a[j + 1]; j += m; }
    return 0;
}
static uint32_t spvi_decl(spvi *c, uint32_t op, const uint32_t *ops, uint32_t nops, int reuse) {   // an existing declaration, or a new one
    uint32_t id = reuse ? spvi_find(c->w, 5, c->fn, op, ops, nops) : 0; if (!id && reuse) id = spvi_find(c->nt, 0, c->nnt, op, ops, nops);
    if (id || c->nnt + nops + 2 > 4096) return id;
    id = c->bound++; c->nt[c->nnt++] = (nops + 2) << 16 | op; c->nt[c->nnt++] = id; memcpy(&c->nt[c->nnt], ops, nops * 4); c->nnt += nops; return id;
}
static uint32_t spvi_scalar(spvi *c, uint32_t k) { uint32_t o[2] = {32, k - 1}; return spvi_decl(c, 21, o, 2, 1); }   // OpTypeInt 32, unsigned (kind 1) / signed (2)
static uint32_t spvi_vec4(spvi *c, uint32_t k) { uint32_t o[2] = {spvi_scalar(c, k), 4}; return spvi_decl(c, 23, o, 2, 1); }
// the type for old with its image's sampled type made uint / int: images, sampled images, arrays of them, pointers to those
static uint32_t spvi_type(spvi *c, uint32_t old, uint32_t k) {
    for (int j = 0; j < c->nmap; j++) if (c->map[j].old == old && c->map[j].kind == k) return c->map[j].id;
    uint32_t i = old < c->bound ? c->at[old] : 0; if (!i) return 0;
    uint32_t n = c->w[i] >> 16, op = c->w[i] & 0xffff, d[12]; if (n > 12 || n < 3) return 0; memcpy(d, &c->w[i + 2], (n - 2) * 4);
    if (op == 25) d[0] = spvi_scalar(c, k);                                                                          // OpTypeImage: the sampled type
    else if (op == 27 || op == 28 || op == 29) { if (!(d[0] = spvi_type(c, d[0], k))) return 0; }                  // OpTypeSampledImage, OpTypeArray, OpTypeRuntimeArray
    else if (op == 32 && n == 4) { if (!(d[1] = spvi_type(c, d[1], k))) return 0; }                                // OpTypePointer
    else return 0;
    uint32_t id = spvi_decl(c, op, d, n - 2, op != 28 && op != 29); if (!id) return 0;
    if (c->nmap < 64) c->map[c->nmap++] = (typeof(c->map[0])){old, k, id};
    return id;
}
static uint32_t *spv_int_images(const uint32_t *w, uint32_t nw, const uint16_t *bind, const uint8_t *kind, int nb, uint32_t *onw) {
    spvi c = {.w = w, .fn = nw, .bound = w[3]};
    for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16; if (!n) return NULL; if ((w[i] & 0xffff) == 54) { c.fn = i; break; } i += n; }   // the first OpFunction
    uint32_t base = c.bound; c.at = calloc(base, sizeof *c.at); c.nt = malloc(4096 * 4);
    uint8_t *taint = calloc(base, 1);   // ids that are (pointers to) the retyped images: their kind
    for (uint32_t i = 5; i < c.fn;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (op >= 19 && op <= 39 && w[i + 1] < base) c.at[w[i + 1]] = i;   // OpType*
        if (op == 71 && n >= 4 && w[i + 2] == 33) for (int k = 0; k < nb; k++) if (w[i + 3] == bind[k] && w[i + 1] < base) taint[w[i + 1]] = kind[k];   // OpDecorate Binding
        i += n; }
    // the functions, rewritten apart: every new type they need is known before the declarations are written
    uint32_t *fb = malloc(((size_t)(nw - c.fn) * 2 + 16) * 4), nf = 0, *mv = malloc(1024 * 4), nmv = 0; int ok = 1;
    for (uint32_t i = 5; i < c.fn && ok;) { uint32_t n = w[i] >> 16;   // the retyped variables, with their new pointer types
        if ((w[i] & 0xffff) == 59 && w[i + 2] < base && taint[w[i + 2]]) { uint32_t t = spvi_type(&c, w[i + 1], taint[w[i + 2]]); if (!t || nmv + n > 1024) ok = 0; else { memcpy(&mv[nmv], &w[i], n * 4); mv[nmv + 1] = t; nmv += n; } }
        i += n; }
    for (uint32_t i = c.fn; i < nw && ok;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n) { ok = 0; break; }
        uint32_t *x = &fb[nf]; memcpy(x, &w[i], n * 4); nf += n;
        uint32_t src = n >= 4 ? w[i + 3] : 0, k = src && src < base ? taint[src] : 0;
        if (k && (op == 61 || op == 65 || op == 66 || op == 83 || op == 100 || op == 86)) {   // Load, AccessChain, InBoundsAccessChain, CopyObject, Image, SampledImage
            uint32_t t = spvi_type(&c, x[1], k); if (!t) { ok = 0; break; } x[1] = t; if (x[2] < base) taint[x[2]] = (uint8_t)k;
        } else if (k && (op == 87 || op == 88 || op == 91 || op == 92 || op == 95 || op == 96)) {   // Sample{Implicit,Explicit}Lod, SampleProj*, Fetch, Gather: a vec4 of the integer type, bitcast back
            uint32_t ft = x[1], r = x[2], r2 = c.bound++; x[1] = spvi_vec4(&c, k); x[2] = r2;
            fb[nf++] = 4u << 16 | 124; fb[nf++] = ft; fb[nf++] = r; fb[nf++] = r2;   // OpBitcast
        }
        i += n; }
    uint32_t *o = NULL, no = 5;
    if (ok && c.nnt < 4096) {
        o = malloc(((size_t)c.fn + c.nnt + nmv + nf + 8) * 4); memcpy(o, w, 20); o[3] = c.bound;
        for (uint32_t i = 5; i < c.fn;) { uint32_t n = w[i] >> 16; if (!((w[i] & 0xffff) == 59 && w[i + 2] < base && taint[w[i + 2]])) { memcpy(&o[no], &w[i], n * 4); no += n; } i += n; }
        memcpy(&o[no], c.nt, c.nnt * 4); no += c.nnt; memcpy(&o[no], mv, nmv * 4); no += nmv; memcpy(&o[no], fb, nf * 4); no += nf;
        *onw = no;
    }
    free(c.at); free(c.nt); free(taint); free(fb); free(mv);
    return o;
}
// Vertex inputs. Maxwell's vertex fetch converts each attribute by its format and leaves 32-bit words in the shader's registers, which the shader reads as it means
// them. The translator declares an input float unless the profile's facts say otherwise, and Vulkan wants an input's numeric type to match its format's: Adreno takes
// a mismatch as the register's bits, MoltenVK (the Android emulator on a Mac) refuses the pipeline ("Vertex attribute type mismatch between host and shader"): Link's
// Awakening's characters (8 vertex shaders in Marin and Tarin's house) went undrawn. A variant declares those inputs with their format's type and bitcasts every load
// back to the type the code reads, which gives the shader the same words. want[location]: 0 keep, 1 uint, 2 int, 3 float (32-bit). NULL when the module is not as expected
static uint32_t spvi_num(spvi *c, uint32_t old, uint32_t k) {   // old with its 32-bit scalar made uint / int / float: scalars, vectors, arrays, pointers to those
    for (int j = 0; j < c->nmap; j++) if (c->map[j].old == old && c->map[j].kind == k) return c->map[j].id;
    uint32_t i = old < c->bound ? c->at[old] : 0; if (!i) return 0;
    uint32_t n = c->w[i] >> 16, op = c->w[i] & 0xffff, d[4], id; if (n > 6 || n < 3) return 0; memcpy(d, &c->w[i + 2], (n - 2) * 4);
    if (op == 21 || op == 22) { if (d[0] != 32) return 0; if (k == 3) { uint32_t o32[1] = {32}; id = spvi_decl(c, 22, o32, 1, 1); } else id = spvi_scalar(c, k); }   // OpTypeInt / OpTypeFloat
    else if (op == 23 || op == 28) { if (!(d[0] = spvi_num(c, d[0], k))) return 0; id = spvi_decl(c, op, d, n - 2, op == 23); }   // OpTypeVector, OpTypeArray
    else if (op == 32 && n == 4) { if (!(d[1] = spvi_num(c, d[1], k))) return 0; id = spvi_decl(c, op, d, 2, 1); }                 // OpTypePointer
    else return 0;
    if (id && c->nmap < 64) c->map[c->nmap++] = (typeof(c->map[0])){old, k, id};
    return id;
}
static uint32_t *spv_retype_inputs(const uint32_t *w, uint32_t nw, const uint8_t want[32], uint32_t *onw) {
    spvi c = {.w = w, .fn = nw, .bound = w[3]};
    for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16; if (!n) return NULL; if ((w[i] & 0xffff) == 54) { c.fn = i; break; } i += n; }   // the first OpFunction
    uint32_t base = c.bound; c.at = calloc(base, sizeof *c.at); c.nt = malloc(4096 * 4);
    uint8_t *loc = calloc(base, 1), *taint = calloc(base, 1);   // ids decorated with a retyped location / (pointers into) the retyped inputs: the kind wanted
    for (uint32_t i = 5; i < c.fn;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (op >= 19 && op <= 39 && w[i + 1] < base) c.at[w[i + 1]] = i;   // OpType*
        if (op == 71 && n == 4 && w[i + 2] == 30 && w[i + 3] < 32 && w[i + 1] < base) loc[w[i + 1]] = want[w[i + 3]];   // OpDecorate Location
        i += n; }
    uint32_t *fb = malloc(((size_t)(nw - c.fn) * 2 + 16) * 4), nf = 0, *mv = malloc(1024 * 4), nmv = 0; int ok = 1, nin = 0;
    for (uint32_t i = 5; i < c.fn && ok;) { uint32_t n = w[i] >> 16;   // the retyped input variables, with their new pointer types
        if ((w[i] & 0xffff) == 59 && n >= 4 && w[i + 3] == 1 && w[i + 2] < base && loc[w[i + 2]]) { uint32_t k = loc[w[i + 2]], t = spvi_num(&c, w[i + 1], k);
            if (!t || nmv + n > 1024) ok = 0; else { memcpy(&mv[nmv], &w[i], n * 4); mv[nmv + 1] = t; nmv += n; taint[w[i + 2]] = (uint8_t)k; nin++; } }
        i += n; }
    for (uint32_t i = c.fn; i < nw && ok;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n) { ok = 0; break; }
        uint32_t *x = &fb[nf]; memcpy(x, &w[i], n * 4); nf += n;
        uint32_t src = n >= 4 ? w[i + 3] : 0, k = src && src < base ? taint[src] : 0;
        if (k && (op == 65 || op == 66 || op == 83)) {   // AccessChain, InBoundsAccessChain, CopyObject: a pointer into the input, retyped the same way
            uint32_t t = spvi_num(&c, x[1], k); if (!t) { ok = 0; break; } x[1] = t; if (x[2] < base) taint[x[2]] = (uint8_t)k;
        } else if (k && op == 61) {   // Load: the retyped value, bitcast back to the type the code reads
            uint32_t ft = x[1], r = x[2], r2 = c.bound++, t = spvi_num(&c, ft, k); if (!t) { ok = 0; break; } x[1] = t; x[2] = r2;
            fb[nf++] = 4u << 16 | 124; fb[nf++] = ft; fb[nf++] = r; fb[nf++] = r2;   // OpBitcast
        } else if (op == 57) { for (uint32_t a = 4; a < n; a++) if (w[i + a] < base && taint[w[i + a]]) ok = 0; }   // FunctionCall with one of them: not handled
        i += n; }
    uint32_t *o = NULL, no = 5;
    if (ok && nin && c.nnt < 4096) {
        o = malloc(((size_t)c.fn + c.nnt + nmv + nf + 8) * 4); memcpy(o, w, 20); o[3] = c.bound;
        for (uint32_t i = 5; i < c.fn;) { uint32_t n = w[i] >> 16; if (!((w[i] & 0xffff) == 59 && w[i + 2] < base && taint[w[i + 2]])) { memcpy(&o[no], &w[i], n * 4); no += n; } i += n; }
        memcpy(&o[no], c.nt, c.nnt * 4); no += c.nnt; memcpy(&o[no], mv, nmv * 4); no += nmv; memcpy(&o[no], fb, nf * 4); no += nf;
        *onw = no;
    }
    free(c.at); free(c.nt); free(loc); free(taint); free(fb); free(mv);
    return o;
}
// A variant of a fragment shader that writes gl_FragDepth = gl_FragCoord.z before each return of its entry point (DepthReplacing): the same depth, tested after the
// shader instead of by Adreno's early culling (gfx_vk_exec.c fdz says when). NULL when the shader writes its depth already or the module is not as expected
static uint32_t *spv_frag_depth(const uint32_t *w, uint32_t nw, uint32_t *onw) {
    spvi c = {.w = w, .fn = nw, .bound = w[3]};
    uint32_t ep = 0, epfn = 0, firsttype = 0, coord = 0, c2 = 0;
    for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n || i + n > nw) return NULL;
        if (op == 15) { if (ep || n < 3 || w[i + 1] != 4) return NULL; ep = i; epfn = w[i + 2]; }   // one entry point, a Fragment one
        if (op == 71 && n >= 4 && w[i + 2] == 11 && w[i + 3] == 22) return NULL;   // BuiltIn FragDepth: written already
        if (op == 71 && n >= 4 && w[i + 2] == 11 && w[i + 3] == 15) coord = w[i + 1];   // BuiltIn FragCoord
        if (!firsttype && ((op >= 19 && op <= 39) || (op >= 41 && op <= 52) || op == 59)) firsttype = i;   // the first type, constant or variable: the annotations end here
        if (op == 54) { c.fn = i; break; }   // the first OpFunction
        i += n; }
    if (!ep || !firsttype || c.fn == nw) return NULL;
    c.nt = malloc(4096 * 4);
    uint32_t o32[1] = {32}; uint32_t f32 = spvi_decl(&c, 22, o32, 1, 1), i32 = spvi_scalar(&c, 2);
    uint32_t ov4[2] = {f32, 4}; uint32_t v4 = spvi_decl(&c, 23, ov4, 2, 1);
    uint32_t op1[2] = {1, v4}, op2[2] = {1, f32}, op3[2] = {3, f32}; uint32_t pin4 = spvi_decl(&c, 32, op1, 2, 1), pinf = spvi_decl(&c, 32, op2, 2, 1), poutf = spvi_decl(&c, 32, op3, 2, 1);
    if (!f32 || !i32 || !v4 || !pin4 || !pinf || !poutf || c.nnt + 16 > 4096) { free(c.nt); return NULL; }
    for (uint32_t i = 5; i < c.fn;) { uint32_t n = w[i] >> 16; if ((w[i] & 0xffff) == 43 && n == 4 && w[i + 1] == i32 && w[i + 3] == 2) c2 = w[i + 2]; i += n; }   // OpConstant int 2
    if (!c2) { c2 = c.bound++; c.nt[c.nnt++] = 4u << 16 | 43; c.nt[c.nnt++] = i32; c.nt[c.nnt++] = c2; c.nt[c.nnt++] = 2; }
    int newcoord = !coord; if (newcoord) { coord = c.bound++; c.nt[c.nnt++] = 4u << 16 | 59; c.nt[c.nnt++] = pin4; c.nt[c.nnt++] = coord; c.nt[c.nnt++] = 1; }   // OpVariable Input
    uint32_t dv = c.bound++; c.nt[c.nnt++] = 4u << 16 | 59; c.nt[c.nnt++] = poutf; c.nt[c.nnt++] = dv; c.nt[c.nnt++] = 3;   // OpVariable Output
    uint32_t nret = 0; for (uint32_t i = c.fn; i < nw;) { uint32_t n = w[i] >> 16; if (!n) { free(c.nt); return NULL; } nret += (w[i] & 0xffff) == 253; i += n; }
    uint32_t *o = malloc(((size_t)nw + c.nnt + 16 + 12 * nret) * 4), no = 5; memcpy(o, w, 20);
    for (uint32_t i = 5; i < c.fn;) { uint32_t n = w[i] >> 16;
        if (i == firsttype) { uint32_t d[8] = {4u << 16 | 71, dv, 11, 22, 4u << 16 | 71, coord, 11, 15}; memcpy(&o[no], d, (newcoord ? 8 : 4) * 4); no += newcoord ? 8 : 4; }   // BuiltIn FragDepth (FragCoord)
        if (i == ep) { o[no++] = (n + 1 + newcoord) << 16 | 15; memcpy(&o[no], &w[i + 1], (n - 1) * 4); no += n - 1; o[no++] = dv; if (newcoord) o[no++] = coord;   // the entry point lists them
                       o[no++] = 3u << 16 | 16; o[no++] = epfn; o[no++] = 12; }   // OpExecutionMode DepthReplacing
        else { memcpy(&o[no], &w[i], n * 4); no += n; }
        i += n; }
    memcpy(&o[no], c.nt, c.nnt * 4); no += c.nnt;
    int in_ep = 0;
    for (uint32_t i = c.fn; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (op == 54) in_ep = w[i + 2] == epfn; else if (op == 56) in_ep = 0;   // OpFunction / OpFunctionEnd
        if (op == 253 && in_ep) { uint32_t a = c.bound++, b = c.bound++, x[12] = {5u << 16 | 65, pinf, a, coord, c2, 4u << 16 | 61, f32, b, a, 3u << 16 | 62, dv, b};   // AccessChain .z, Load, Store
                                  memcpy(&o[no], x, sizeof x); no += 12; }
        memcpy(&o[no], &w[i], n * 4); no += n; i += n; }
    o[3] = c.bound; *onw = no; free(c.nt); return o;
}
// DM_VK_SPV_DIR=<dir> with DM_VK_SPV_ALT=1 (live, debugging): pipelines made while it is on use <dir>/<key>.spv in place of the bundle's code where the file
// exists (same bindings), so two compilations of a shader can be compared in one run: the translator's module through spirv-opt, its GLSL through glslc...
static VkShaderModule alt_module(vkshader_t *s) {
    const char *dir = getenv("DM_VK_SPV_DIR"); char path[512]; if (!dir) return s->mod;
    snprintf(path, sizeof path, "%s/%016llx.spv", dir, (unsigned long long)s->key); FILE *f = fopen(path, "rb"); if (!f) return s->mod;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); uint32_t *w = n > 20 && !(n & 3) ? malloc((size_t)n) : NULL; VkShaderModule m = s->mod;
    if (w && fread(w, 1, (size_t)n, f) == (size_t)n) { VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, (size_t)n, w};
        for (uint32_t i = 5, nw = (uint32_t)(n / 4); i < nw;) { uint32_t k = w[i] >> 16; if (!k) break; if ((w[i] & 0xffff) == 71 && k >= 4 && w[i + 2] == 34) w[i + 3] = (uint32_t)s->set; i += k; }   // the stage's set, as at load
        if (vkCreateShaderModule(V.dev, &ci, NULL, &m)) m = s->mod; else VKLOG("shader %016llx: alternative code from %s", (unsigned long long)s->key, path); }
    free(w); fclose(f); return m;
}
// A fragment shader's dual-source variant (gfx_vk_exec.c pkey dual): its output at location 1 becomes location 0, index 1, the second source of target 0's blend.
// NULL when it has no output at location 1, or outputs past it (one dual-source attachment at most: maxFragmentDualSrcAttachments)
static uint32_t *spv_dual(const uint32_t *w, uint32_t nw, uint32_t *onw) {
    uint32_t bound = w[3], fn = nw, at = 0, v1 = 0; uint8_t *out = calloc(bound, 1); int past = 0;
    for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n || i + n > nw) { free(out); return NULL; }
        if (op == 54) { fn = i; break; }
        if (op == 59 && n >= 4 && w[i + 3] == 3 && w[i + 2] < bound) out[w[i + 2]] = 1;   // OpVariable ... Output
        i += n; }
    for (uint32_t i = 5; i < fn;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (op == 71 && n == 4 && w[i + 2] == 30 && w[i + 1] < bound && out[w[i + 1]]) { if (w[i + 3] == 1) { at = i; v1 = w[i + 1]; } else if (w[i + 3] > 1) past = 1; }   // Location
        if (op == 71 && n == 4 && w[i + 2] == 32) past = 1;   // an Index already: not a shader this is for
        i += n; }
    free(out);
    if (!at || past) return NULL;
    uint32_t *o = malloc(((size_t)nw + 4) * 4); memcpy(o, w, (size_t)(at + 4) * 4); o[at + 3] = 0;   // Location 1 → 0
    o[at + 4] = 4u << 16 | 71; o[at + 5] = v1; o[at + 6] = 32; o[at + 7] = 1;   // OpDecorate ... Index 1
    memcpy(&o[at + 8], &w[at + 4], (size_t)(nw - at - 4) * 4); *onw = nw + 4;
    return o;
}
VkShaderModule vk_shader_module(vkshader_t *s, uint64_t um, uint64_t im, uint32_t xo, int fd, uint64_t ri) {
    int dual = fd >> 1 & 1; fd &= 1;   // fd bit 1: the dual-source variant
    if (fd && (s->stage != 1 || s->depth_repl)) fd = 0;
    if (dual && s->stage != 1) dual = 0;
    if (s->stage != 0) ri = 0;
    if (!(um | im | xo | (uint32_t)fd | (uint32_t)dual | ri) || !s->spv) return s->mod;
    if (xo == 1u << 31) {   // DM_VK_SPV_ALT
        pthread_mutex_lock(&g_sh_mu); for (int i = 0; i < s->nvar; i++) if (s->var[i].um == 0 && s->var[i].im == 0 && s->var[i].xo == xo) { VkShaderModule m = s->var[i].mod; pthread_mutex_unlock(&g_sh_mu); return m; }
        VkShaderModule m = alt_module(s); if (s->nvar < 8) s->var[s->nvar++] = (typeof(s->var[0])){0, 0, 0, xo, 0, m}; pthread_mutex_unlock(&g_sh_mu); return m; }
    xo &= ~(1u << 31);
    pthread_mutex_lock(&g_sh_mu);
    for (int i = 0; i < s->nvar; i++) if (s->var[i].um == um && s->var[i].im == im && s->var[i].ri == ri && s->var[i].xo == xo && s->var[i].fd == (fd | dual << 1)) { VkShaderModule m = s->var[i].mod; pthread_mutex_unlock(&g_sh_mu); return m; }
    uint16_t bind[64]; uint8_t kind[64]; int nb = 0;
    for (int i = 0; i < s->nbind && i < 64; i++) if ((um | im) >> i & 1) { bind[nb] = s->b[i].binding; kind[nb++] = um >> i & 1 ? 1 : 2; }
    uint32_t nw = s->nspv, *w = NULL; VkShaderModule m = s->mod; int miss = 0;   // miss: a step that should have made a variant could not
    if (ri) { uint8_t want[32]; for (int l = 0; l < 32; l++) want[l] = (uint8_t)(ri >> 2 * l & 3); w = spv_retype_inputs(s->spv, s->nspv, want, &nw); miss = !w; }
    if (nb && !miss) { uint32_t n2 = 0, *w2 = spv_int_images(w ? w : s->spv, w ? nw : s->nspv, bind, kind, nb, &n2); free(w); w = w2; nw = n2; miss = !w; }
    if (xo && !miss) { uint32_t n2 = 0, *w2 = spv_add_outputs(w ? w : s->spv, w ? nw : s->nspv, xo, &n2); free(w); w = w2; nw = n2; miss = !w;
        if (w && s->stage == 0) { uint32_t n3 = 0, *w3 = spv_vs_init(w, nw, &n3); if (w3) { free(w); w = w3; nw = n3; } } }   // the added outputs start as (0, 0, 0, 1) too
    if (fd && !miss) { uint32_t n2 = 0, *w2 = spv_frag_depth(w ? w : s->spv, w ? nw : s->nspv, &n2); free(w); w = w2; nw = n2; miss = !w; }
    if (dual && !miss) { uint32_t n2 = 0, *w2 = spv_dual(w ? w : s->spv, w ? nw : s->nspv, &n2); free(w); w = w2; nw = n2; miss = !w; }
    if (w && !miss) { VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, (size_t)nw * 4, w};
        if (vkCreateShaderModule(V.dev, &ci, NULL, &m)) { m = s->mod; VKLOG("[RED] shader %016llx: its variant was refused (bindings %#llx / %#llx, outputs %#x, fragment depth %d, dual source %d, inputs %#llx)", (unsigned long long)s->key, (unsigned long long)um, (unsigned long long)im, xo, fd, dual, (unsigned long long)ri); }
        else VKLOG("shader %016llx: variant for bindings %#llx (uint texels) %#llx (int texels), outputs added %#x, fragment depth written %d, dual source %d, inputs retyped %#llx", (unsigned long long)s->key, (unsigned long long)um, (unsigned long long)im, xo, fd, dual, (unsigned long long)ri); }
    else VKLOG("[RED] shader %016llx: no variant could be made (bindings %#llx / %#llx, outputs %#x, fragment depth %d, dual source %d, inputs %#llx)", (unsigned long long)s->key, (unsigned long long)um, (unsigned long long)im, xo, fd, dual, (unsigned long long)ri);
    free(w);
    if (s->nvar < 8) s->var[s->nvar++] = (typeof(s->var[0])){um, im, ri, xo, fd | dual << 1, m};
    pthread_mutex_unlock(&g_sh_mu);
    return m;
}
// static buffers: Ryujinx's support buffer (render scales 1.0, nothing else set: as nvn_mtl.m support_buf) and 64 KB of zeros (unbound uniform slots, driver segments nobody reads)
static VkBuffer static_buf(int which) {
    static VkBuffer b[2]; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    { VkBuffer r = __atomic_load_n(&b[which], __ATOMIC_ACQUIRE); if (r) return r; }   // made already: no lock (every draw binds them)
    pthread_mutex_lock(&mu);
    if (!b[which]) {
        VkBuffer nb; VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE};
        VKCHK(vkCreateBuffer(V.dev, &bi, NULL, &nb)); VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, nb, &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
        VkDeviceMemory m; VKCHK(vkAllocateMemory(V.dev, &ai, NULL, &m)); VKCHK(vkBindBufferMemory(V.dev, nb, m, 0));
        void *p; VKCHK(vkMapMemory(V.dev, m, 0, VK_WHOLE_SIZE, 0, &p)); memset(p, 0, 65536);
        if (which == 0) for (int i = 0; i < 73; i++) *(float *)((uint8_t *)p + 192 + 16 * i) = 1.0f;   // render_scale[i].x (SupportBuffer.cs layout)
        __atomic_store_n(&b[which], nb, __ATOMIC_RELEASE);   // published once filled
    }
    pthread_mutex_unlock(&mu);
    return b[which];
}
VkBuffer vk_support_buffer(void) { return static_buf(0); }
VkBuffer vk_zero_buffer(void) { return static_buf(1); }
// a shader's c1 constants: copied once into a buffer of their own (the bundle bytes do not change)
void vk_shader_c1(vkshader_t *s, VkBuffer *buf, VkDeviceSize *off, VkDeviceSize *len) {
    if (!s->c1_len) { *buf = vk_zero_buffer(); *off = 0; *len = 65536; return; }
    if (!s->c1_buf) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (s->c1_len + 255) & ~255u, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE};
        VKCHK(vkCreateBuffer(V.dev, &bi, NULL, &s->c1_buf)); VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, s->c1_buf, &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
        VkDeviceMemory m; VKCHK(vkAllocateMemory(V.dev, &ai, NULL, &m)); VKCHK(vkBindBufferMemory(V.dev, s->c1_buf, m, 0));
        void *p; VKCHK(vkMapMemory(V.dev, m, 0, VK_WHOLE_SIZE, 0, &p)); memset(p, 0, bi.size); memcpy(p, s->c1, s->c1_len);
    }
    *buf = s->c1_buf; *off = 0; *len = (s->c1_len + 255) & ~255u;
}
// pipeline layouts by the set layouts of their stages (an empty layout fills the sets of absent stages)
VkPipelineLayout vk_pipeline_layout(VkDescriptorSetLayout sets[5], int nsets) {
    static VkDescriptorSetLayout empty; static nvn_hmap cache = {.klen = 5 * sizeof(VkDescriptorSetLayout)}; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    if (!empty) { VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; VKCHK(vkCreateDescriptorSetLayout(V.dev, &li, NULL, &empty)); }
    VkDescriptorSetLayout k[5]; for (int i = 0; i < 5; i++) { k[i] = i < nsets && sets[i] ? sets[i] : (i < nsets ? empty : VK_NULL_HANDLE); if (i < nsets) sets[i] = k[i]; }   // absent stages get the empty layout (the caller allocates their sets from it)
    VkPipelineLayout pl = nvn_hm_get(&cache, k);
    if (!pl) {
        VkPipelineLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, (uint32_t)nsets, k};
        VKCHK(vkCreatePipelineLayout(V.dev, &ci, NULL, &pl)); nvn_hm_put(&cache, k, pl);
    }
    pthread_mutex_unlock(&mu);
    return pl;
}
