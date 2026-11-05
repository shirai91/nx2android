// Android port, Vulkan backend: subgroup operations a shader stage lacks, rewritten as subgroups of one invocation (see gfx_vk.h; called by gfx_vk_shader.c vk_shader).
// Plain C over the SPIR-V words, without Vulkan, so tests/test_vk_one_lane.py builds and checks it on the PC.
//
// The translator writes Maxwell's warp votes, ballots and shuffles as Vulkan subgroup operations (for a subgroup of up to 128 lanes: shader_vk.py SUBGROUP), in
// any stage. Vulkan allows them only in the stages VkPhysicalDeviceSubgroupProperties::supportedStages lists (VUID-RuntimeSpirv-None-06343), of the classes
// supportedOperations lists, and quad operations outside fragment and compute only with quadOperationsInAllStages (-06342). Turnip lists graphics stages only on
// a GPU with getfiberid (tu_device.cc): on an Adreno 650 (a6xx gen3) supportedStages is compute alone, yet its compiler still emitted fiber ids and hardware
// shuffles in a fragment shader (guarded by an assert only), and Pokemon Pokopia's 541aa7b27f1fae4d (the translator's SHFL helper, which reads the first lane its
// ballot found) lost the device at the title screen on a Retroid Pocket 5. Banana Native reports supportedStages 0xf0 (fragment, compute, task, mesh) from
// Qualcomm's driver on an Adreno 840, where its Donkey Kong Bananza geometry shaders lost triangles the same way.
// A module that uses a class its stage lacks is rewritten as subgroups of one invocation, as upstream's Apple chain does for Metal's vertex functions
// (dm/gfx/tools/dmsh_build.py spv_single_lane): the invocation's id is 0, the subgroup size 1, a ballot is its predicate in bit 0, the equal / greater-or-equal /
// less-or-equal masks are bit 0 and the greater / less masks 0, a shuffle, broadcast or quad operation returns the invocation's own value, all / any return the
// predicate, all-equal and elect are true. That is exact for warp code that finds its lanes with a ballot (the scalarizing patterns met: one lane does the work
// for those that agree); code that assumes all 32 lanes are there (a butterfly sum) gets the lane's own share. The built-in variables become Private variables
// that hold those values, so loads through access chains read them too (Legends: Z-A's compute shaders read SubgroupLtMask a word at a time), and the
// subgroup capabilities and extensions go. Ballot counts and finds, inverse ballots, arithmetic, rotates and partitions are not covered (none in Pokopia's
// or Legends: Z-A's bundles, which use all, any, ballot, shuffle, the invocation ID and the less mask): such a module is left as it is, and *why names the opcode.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t *spv_one_lane(const uint32_t *w, uint32_t nw, uint32_t ops, uint32_t *onw, char *why, size_t nwhy);

// the class a subgroup opcode needs (VkSubgroupFeatureFlagBits: basic 1, vote 2, arithmetic 4, ballot 8, shuffle 0x10, shuffle relative 0x20, clustered 0x40,
// quad 0x80, partitioned 0x100, rotate 0x200), 0 for any other opcode; *to: what it becomes in a subgroup of one ('v' its operand *arg, 't' true, 'b' its predicate
// *arg as bit 0 of a ballot, 0 not covered)
static uint32_t sg_op(uint32_t op, char *to, uint32_t *arg) {
    *to = 0; *arg = 4;   // OpGroupNonUniform*: result type, result, scope, then the value or predicate
    switch (op) {
    case 333: *to = 't'; return 1;                          // Elect
    case 334: case 335: *to = 'v'; return 2;                // All, Any: the predicate
    case 336: *to = 't'; return 2;                          // AllEqual
    case 337: case 338: *to = 'v'; return 8;                // Broadcast, BroadcastFirst
    case 339: *to = 'b'; return 8;                          // Ballot
    case 345: case 346: *to = 'v'; return 0x10;             // Shuffle, ShuffleXor
    case 347: case 348: *to = 'v'; return 0x20;             // ShuffleUp, ShuffleDown
    case 365: case 366: *to = 'v'; return 0x80;             // QuadBroadcast, QuadSwap
    case 4421: *to = 'b'; *arg = 3; return 8;               // SPV_KHR_shader_ballot and _subgroup_vote (no scope operand): SubgroupBallotKHR,
    case 4422: case 4432: *to = 'v'; *arg = 3; return 8;    // SubgroupFirstInvocationKHR, SubgroupReadInvocationKHR,
    case 4428: case 4429: *to = 'v'; *arg = 3; return 2;    // SubgroupAllKHR, SubgroupAnyKHR,
    case 4430: *to = 't'; return 2;                         // SubgroupAllEqualKHR
    }
    return op >= 340 && op <= 344 ? 8 : op >= 349 && op <= 364 ? 4 : op == 4431 ? 0x200 : op == 5296 ? 0x100 : op == 5110 || op == 5111 ? 0x80 : 0;   // inverse ballot, bit extract / count, find LSB / MSB; arithmetic; RotateKHR; PartitionNV; QuadAll / QuadAnyKHR
}
// what a subgroup built-in holds in a subgroup of one: 1 the value 1 (SubgroupSize), 2 bit 0 (SubgroupEq / Ge / LeMask), 3 zero (SubgroupLocalInvocationId,
// SubgroupGt / LtMask), -1 not covered (SubgroupMaxSize, NumSubgroups, SubgroupId: compute only, which every device gives subgroups), 0 not a subgroup built-in
static int sg_builtin(uint32_t b) {
    switch (b) { case 36: return 1; case 4416: case 4417: case 4419: return 2; case 41: case 4418: case 4420: return 3; case 37: case 38: case 40: return -1; default: return 0; }
}
// per id: a pointer type's storage class and pointee, a vector type's component type and width, a 32-bit integer type; the built-in a variable is, mark 1 for
// its variable and 2 for a pointer into it; and per type the declarations made for it: a Private pointer to it, constants 0, 1, null, true and bit 0
typedef struct { uint32_t to, comp, priv, k0, k1, kn, kt, kb; uint16_t sc, bi; uint8_t ncomp, isint, mark; } sgid;
typedef struct { sgid *t; uint32_t nb, *d, nd, cap; int oom; } sgx;   // the ids, the next free id, the new declarations
static void sgx_put(sgx *c, const uint32_t *x, uint32_t n) {
    if (c->nd + n > c->cap) { uint32_t cap = (c->cap + n) * 2, *d = realloc(c->d, (size_t)cap * 4); if (!d) { c->oom = 1; return; } c->d = d; c->cap = cap; }
    memcpy(c->d + c->nd, x, (size_t)n * 4); c->nd += n;
}
static uint32_t k_int(sgx *c, uint32_t ty, uint32_t v) {   // OpConstant ty 0 / 1
    uint32_t *k = v ? &c->t[ty].k1 : &c->t[ty].k0;
    if (!*k) { *k = c->nb++; uint32_t x[4] = {4u << 16 | 43, ty, *k, v}; sgx_put(c, x, 4); }
    return *k;
}
static uint32_t k_null(sgx *c, uint32_t ty) { if (!c->t[ty].kn) { c->t[ty].kn = c->nb++; uint32_t x[3] = {3u << 16 | 46, ty, c->t[ty].kn}; sgx_put(c, x, 3); } return c->t[ty].kn; }   // OpConstantNull
static uint32_t k_true(sgx *c, uint32_t ty) { if (!c->t[ty].kt) { c->t[ty].kt = c->nb++; uint32_t x[3] = {3u << 16 | 41, ty, c->t[ty].kt}; sgx_put(c, x, 3); } return c->t[ty].kt; }   // OpConstantTrue
static uint32_t k_bit0(sgx *c, uint32_t ty) {   // OpConstantComposite (1, 0, …) of an integer vector type
    sgid *v = &c->t[ty];
    if (!v->kb) { uint32_t one = k_int(c, v->comp, 1), zero = k_int(c, v->comp, 0); v->kb = c->nb++;
        uint32_t x[7] = {(3u + v->ncomp) << 16 | 44, ty, v->kb, one, zero, zero, zero}; sgx_put(c, x, 3u + v->ncomp); }
    return v->kb;
}
static uint32_t p_priv(sgx *c, uint32_t ty) { if (!c->t[ty].priv) { c->t[ty].priv = c->nb++; uint32_t x[4] = {4u << 16 | 32, c->t[ty].priv, 6, ty}; sgx_put(c, x, 4); } return c->t[ty].priv; }   // OpTypePointer Private
static int str_end(uint32_t x) { return !(x & 0xff) || !(x & 0xff00) || !(x & 0xff0000) || !(x & 0xff000000); }   // the word holding a literal string's nul

// The module w (nw words) as subgroups of one invocation, when it uses a subgroup class not in ops (the stage's: 0 when the device has none in it); malloc'd, *onw
// words. NULL when it uses none the stage lacks (why empty), or something not covered (why names it): either way the module stays as it is
uint32_t *spv_one_lane(const uint32_t *w, uint32_t nw, uint32_t ops, uint32_t *onw, char *why, size_t nwhy) {
    if (nwhy) why[0] = 0;
    *onw = 0;
    if (nw < 5 || w[0] != 0x07230203 || w[3] > 1u << 22) return NULL;   // (a module that is not SPIR-V is the driver's to refuse)
    uint32_t used = 0, bad = 0, badbi = 0, nops = 0, a; char to;
    for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n || i + n > nw) return NULL;
        uint32_t cl = sg_op(op, &to, &a); if (cl) { used |= cl; nops++; if ((!to || (to != 't' && n <= a)) && !bad) bad = op; }
        if (op == 71 && n >= 4 && w[i + 2] == 11) { int k = sg_builtin(w[i + 3]); if (k) used |= w[i + 3] >= 4416 ? 8 : 1; if (k < 0 && !badbi) badbi = w[i + 3]; }   // OpDecorate BuiltIn
        i += n; }
    if (!(used & ~ops)) return NULL;
    if (bad || badbi) { snprintf(why, nwhy, bad ? "opcode %u" : "built-in %u", bad ? bad : badbi); return NULL; }
    uint32_t bound = w[3], fn = 0, *o = NULL, no = 5;
    sgx c = {.t = calloc(bound, sizeof(sgid)), .nb = bound}; sgid *t = c.t;
    if (!t) return NULL;
#define ID(k) (n > (k) && x[k] < bound)
    // the types, the built-ins' variables (each a Private one among the new declarations), the pointers into them, and the constants the operations become
    for (uint32_t i = 5; i < nw; i += w[i] >> 16) { const uint32_t *x = &w[i]; uint32_t n = x[0] >> 16, op = x[0] & 0xffff;
        if (op == 54 && !fn) fn = i;
        if (op == 21 && ID(1) && n >= 4) t[x[1]].isint = x[2] == 32;   // OpTypeInt 32
        else if (op == 23 && ID(1) && ID(2) && n >= 4) { t[x[1]].comp = x[2]; t[x[1]].ncomp = x[3] <= 4 ? (uint8_t)x[3] : 0; }   // OpTypeVector
        else if (op == 32 && ID(1) && ID(3)) { t[x[1]].sc = (uint16_t)x[2]; t[x[1]].to = x[3]; if (x[2] == 6 && !t[x[3]].priv) t[x[3]].priv = x[1]; }   // OpTypePointer (a Private one is reused)
        else if (op == 43 && n == 4 && ID(1) && ID(2) && t[x[1]].isint && x[3] <= 1) { uint32_t *k = x[3] ? &t[x[1]].k1 : &t[x[1]].k0; if (!*k) *k = x[2]; }   // OpConstant 0 / 1 (reused)
        else if (op == 41 && ID(1) && ID(2) && !t[x[1]].kt) t[x[1]].kt = x[2];   // OpConstantTrue (reused)
        else if (op == 71 && n >= 4 && x[2] == 11 && ID(1) && sg_builtin(x[3]) > 0) t[x[1]].bi = (uint16_t)x[3];
        else if (op == 59 && ID(1) && ID(2) && t[x[2]].bi) {   // OpVariable of a built-in: Input, of its type
            uint32_t ty = t[x[1]].to, k = (uint32_t)sg_builtin(t[x[2]].bi), init = 0;
            if (n >= 4 && x[3] == 1 && t[x[1]].sc == 1 && ty < bound)
                init = k == 1 && t[ty].isint ? k_int(&c, ty, 1) : k == 3 ? k_null(&c, ty) : k == 2 && t[ty].ncomp && t[t[ty].comp].isint ? k_bit0(&c, ty) : 0;
            if (!init) { snprintf(why, nwhy, "built-in %u of an unexpected type", t[x[2]].bi); goto out; }
            uint32_t v[5] = {5u << 16 | 59, p_priv(&c, ty), x[2], 6, init}; sgx_put(&c, v, 5); t[x[2]].mark = 1; }
        else if ((op == 65 || op == 66 || op == 83) && ID(1) && ID(2) && ID(3) && t[x[3]].mark) {   // OpAccessChain / InBoundsAccessChain / CopyObject of a pointer into one
            if (t[x[1]].sc != 1 || t[x[1]].to >= bound) { snprintf(why, nwhy, "opcode %u into built-in", op); goto out; }
            p_priv(&c, t[x[1]].to); t[x[2]].mark = 2; }
        else if (op == 57 || op == 169 || op == 245) { for (uint32_t k = 3; k < n; k++) if (x[k] < bound && t[x[k]].mark) { snprintf(why, nwhy, "opcode %u with a built-in's pointer", op); goto out; } }   // a call, select or phi of one
        else if (sg_op(op, &to, &a)) {
            if (to == 't') { if (!ID(1)) { snprintf(why, nwhy, "opcode %u of an unexpected type", op); goto out; } k_true(&c, x[1]); }
            else if (to == 'b') { if (!ID(1) || t[x[1]].ncomp != 4 || t[x[1]].comp >= bound || !t[t[x[1]].comp].isint) { snprintf(why, nwhy, "opcode %u of an unexpected type", op); goto out; }
                                  k_int(&c, t[x[1]].comp, 1); k_int(&c, t[x[1]].comp, 0); } } }
    if (!fn || c.oom || !(o = malloc(((size_t)nw + c.nd + 9 * nops) * 4))) goto out;   // (a ballot grows by 9 words at most)
    memcpy(o, w, 20);
    for (uint32_t i = 5; i < nw; i += w[i] >> 16) { const uint32_t *x = &w[i]; uint32_t n = x[0] >> 16, op = x[0] & 0xffff;
        if (i == fn) { memcpy(o + no, c.d, (size_t)c.nd * 4); no += c.nd; }   // the new declarations, after every type and constant they name
        if (op == 17 && n == 2 && ((x[1] >= 61 && x[1] <= 68) || x[1] == 4423 || x[1] == 4431)) continue;   // OpCapability GroupNonUniform*, SubgroupBallotKHR, SubgroupVoteKHR
        if (op == 10 && n >= 2 && (!strncmp((const char *)&x[1], "SPV_KHR_shader_ballot", (n - 1) * 4) || !strncmp((const char *)&x[1], "SPV_KHR_subgroup_vote", (n - 1) * 4))) continue;
        if ((op == 71 || op == 332 || op == 5632) && ID(1) && t[x[1]].mark == 1) continue;   // the built-in's decorations (BuiltIn, Flat)
        if (op == 59 && ID(2) && t[x[2]].mark == 1) continue;   // its Input variable
        if (op == 74) for (uint32_t k = 2; k < n; k++) if (x[k] < bound && t[x[k]].mark == 1) { snprintf(why, nwhy, "opcode 74 naming a built-in"); free(o); o = NULL; goto out; }   // OpGroupDecorate
        if (op == 15 && w[1] < 0x10400) {   // OpEntryPoint: before SPIR-V 1.4 its interface lists Input and Output variables only
            uint32_t k = 3, at = no++; while (k < n && !str_end(x[k++])) {}
            memcpy(o + no, x + 1, (size_t)(k - 1) * 4); no += k - 1;
            for (; k < n; k++) if (!(x[k] < bound && t[x[k]].mark == 1)) o[no++] = x[k];
            o[at] = (no - at) << 16 | 15; continue; }
        if ((op == 65 || op == 66 || op == 83) && ID(2) && t[x[2]].mark == 2) { memcpy(o + no, x, (size_t)n * 4); o[no + 1] = t[t[x[1]].to].priv; no += n; continue; }   // a pointer into it: Private
        if (sg_op(op, &to, &a)) {
            // a ballot: OpSelect(predicate, 1, 0) into word 0 of an OpCompositeConstruct (a scalar condition with vector results is SPIR-V 1.4's, and the translator writes 1.3)
            if (to == 'b') { uint32_t ct = t[x[1]].comp, k0 = t[ct].k0, b = c.nb++, y[13] = {6u << 16 | 169, ct, b, x[a], t[ct].k1, k0, 7u << 16 | 80, x[1], x[2], b, k0, k0, k0};
                             memcpy(o + no, y, sizeof y); no += 13; }
            else { uint32_t y[4] = {4u << 16 | 83, x[1], x[2], to == 't' ? t[x[1]].kt : x[a]}; memcpy(o + no, y, 16); no += 4; }   // OpCopyObject of the value, or true
            continue; }
        memcpy(o + no, x, (size_t)n * 4); no += n; }
    o[3] = c.nb; *onw = no;
#undef ID
out:
    free(t); free(c.d);
    return o;
}
