"""Driver copies must match exactly, including when a test hash always collides.

The harness runs production segment construction, cache lookup and scratch allocation.
Vulkan allocation calls are mocked; no natural hash collision or GPU is involved.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm'


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2] + '\n'


def harness():
    source = (DM / 'android/gfx_vk_exec.c').read_text()
    core = (DM / 'android/gfx_vk_core.c').read_text()
    cache = next(line for line in source.splitlines() if 'g_drvc[DRVC];' in line)
    scratch_type = next(line for line in core.splitlines() if line.endswith('} scratch_blk;'))
    scratch_size = next(line for line in core.splitlines() if line.startswith('#define SCRATCH_BLOCK '))
    hash_fn = function(source, 'static inline uint64_t fnv(').replace('fnv(', 'production_fnv(', 1)
    return r'''
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct { VkDevice dev; VkPhysicalDeviceProperties props; } V;
typedef struct { int stage, index; uint64_t addr, size; } p_bind_buf;
static struct { p_bind_buf ssbo[6][16], ubo[6][16]; uint64_t tex[6][256], img[6][8], samp[6][128]; } S;
static uint32_t g_dispatch[3], g_draw_binst;
static uint64_t g_set_epoch = 1;
static int g_x_nodrvc;
enum { DRVC = 1024 };
''' + cache + '\n' + scratch_type + '\n' + scratch_size + r'''
/* Only the slot fields vk_scratch uses; mapped bytes are owned by mocked Vulkan. */
typedef struct { scratch_blk *blk; int nblk, capblk; } vksub_t;
static vksub_t slot;
static vksub_t *cur(void) { return &slot; }
static struct allocation { VkDeviceSize size; void *memory; } *buffers[64];
static size_t nbuffer, metadata_size;
static const void *forbidden_compare;
static unsigned comparisons;
static size_t expected_compare;
static int compare_bytes(const void *a, const void *b, size_t n) {
    assert(a != forbidden_compare && b != forbidden_compare);
    if (expected_compare) assert(n == expected_compare);
    for (size_t i=0;i<nbuffer;i++) {
        uintptr_t lo=(uintptr_t)buffers[i]->memory, hi=lo+buffers[i]->size;
        assert(!((uintptr_t)a>=lo && (uintptr_t)a<hi));
        assert(!((uintptr_t)b>=lo && (uintptr_t)b<hi));
    }
    comparisons++;
    return memcmp(a, b, n);
}
/* Force metadata to move when production grows its array. Byte mappings stay put. */
static void *move_metadata(void *old, size_t size) {
    void *p = malloc(size); assert(p);
    if (old) memcpy(p, old, metadata_size);
    free(old); metadata_size = size; return p;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *ci,
    const VkAllocationCallbacks *callbacks, VkBuffer *buffer) {
    (void)device; (void)callbacks; assert(nbuffer < 64);
    struct allocation *a = calloc(1, sizeof *a); assert(a); a->size = ci->size;
    buffers[nbuffer++] = a; *buffer = (VkBuffer)(uintptr_t)a; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer, VkMemoryRequirements *req) {
    (void)device; struct allocation *a = (void *)(uintptr_t)buffer;
    *req = (VkMemoryRequirements){a->size, 256, 1};
}
static uint32_t vk_mem_type(uint32_t bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) {
    assert(bits == 1 && required == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) && !preferred);
    return 0;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *ai,
    const VkAllocationCallbacks *callbacks, VkDeviceMemory *memory) {
    (void)device; (void)callbacks; void *p = malloc((size_t)ai->allocationSize); assert(p);
    *memory = (VkDeviceMemory)(uintptr_t)p; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    (void)device; assert(!offset); ((struct allocation *)(uintptr_t)buffer)->memory = (void *)(uintptr_t)memory;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
    VkDeviceSize size, VkMemoryMapFlags flags, void **data) {
    (void)device; assert(!offset && size == VK_WHOLE_SIZE && !flags); *data = (void *)(uintptr_t)memory; return VK_SUCCESS;
}
#define POOL_USAGE (VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
#define VKCHK(call) assert((call) == VK_SUCCESS)
#define realloc move_metadata
''' + function(core, 'void *vk_scratch(') + r'''
#undef realloc
''' + hash_fn + r'''
/* An ordinary unit-test stub deliberately makes unequal inputs hash alike. */
static int force_hash = 1;
static uint64_t forced_hash = 7;
static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    return force_hash ? forced_hash : production_fnv(h, p, n);
}
''' + function(source, 'static uint64_t c2_handle(') + function(source, 'static void drv_segment(') + r'''
#define memcmp compare_bytes
''' + function(source, 'static void drv_shared(') + r'''
#undef memcmp
static void clean(void) {
    memset(g_drvc, 0, sizeof g_drvc);
    for (size_t i = 0; i < nbuffer; i++) { free(buffers[i]->memory); free(buffers[i]); }
    free(slot.blk); memset(&slot, 0, sizeof slot); nbuffer = metadata_size = 0;
    memset(&S, 0, sizeof S); memset(g_dispatch, 0, sizeof g_dispatch); g_draw_binst = 0;
    forbidden_compare = NULL; comparisons = 0; expected_compare = 0; g_x_nodrvc = 0; force_hash = 1; forced_hash = 7; g_set_epoch++;
}
static VkDeviceSize used(void) {
    VkDeviceSize total = 0; for (int i = 0; i < slot.nblk; i++) total += slot.blk[i].used; return total;
}
typedef struct { VkBuffer b; VkDeviceSize o; } ref;
static ref get(int st, int bank, int dyn) { ref r; drv_shared(st, bank, &r.b, &r.o, dyn); return r; }
static int same(ref a, ref b) { return a.b == b.b && a.o == b.o; }
static uint8_t *bytes(ref r) { return (uint8_t *)((struct allocation *)(uintptr_t)r.b)->memory + r.o; }
static void word32(uint8_t *d, unsigned o, uint32_t v) { memcpy(d + o, &v, 4); }
static void word64(uint8_t *d, unsigned o, uint64_t v) { memcpy(d + o, &v, 8); }
/* Expected segments below are hand-built from the documented driver offsets. */
static void changed(unsigned which) {
    clean(); int st = 0, bank = 0, dyn = 0;
    if (which == 2) st = 1;
    if (which >= 3 && which <= 9) st = 5;
    if (which >= 10) bank = 2;
    if (which >= 14) dyn = 1;
    uint8_t before[0x1000] = {0}, after[0x1000];
    if (!bank && st != 5) word32(before, 0x610, 0x44ffe000); /* 2047.0f */
    memcpy(after, before, sizeof after);
    ref first = get(st, bank, dyn); assert(!memcmp(bytes(first), before, sizeof before));
    VkDeviceSize allocated = used(); ref equal = get(st, bank, dyn);
    assert(same(first, equal) && used() == allocated);
    switch (which) {
    case 0: g_draw_binst = 2; word32(after, 0x644, 2); break;
    case 1: S.ssbo[0][0].addr = 0x10000; word64(after, 0x110, 0x10000); break;
    case 2: S.ssbo[1][15].size = 64; word32(after, 0x608, 64); break;
    case 3: g_dispatch[0] = 3; word32(after, 0x200, 3); break;
    case 4: g_dispatch[1] = 4; word32(after, 0x204, 4); break;
    case 5: g_dispatch[2] = 5; word32(after, 0x208, 5); break;
    case 6: S.ubo[5][15].addr = 0x20000; word64(after, 0x300, 0x20000); break;
    case 7: S.ubo[5][15].size = 128; word32(after, 0x308, 128); break;
    case 8: S.ssbo[5][15].addr = 0x30000; word64(after, 0x400, 0x30000); break;
    case 9: S.ssbo[5][15].size = 256; word32(after, 0x408, 256); break;
    case 10: S.tex[0][0] = 10; word64(after, 0x20, 10); break;
    case 11: S.tex[0][255] = 11; word64(after, 0x560, 11); break;
    case 12: S.img[0][7] = 12; word64(after, 0x158, 12); break;
    case 13: S.samp[0][127] = 13u << 20; word64(after, 0x960, 13u << 20); break;
    case 14: S.tex[0][31] = 14 | (2u << 20); word32(after, 0x118, 31 | (31u << 20)); break;
    case 15: S.samp[0][127] = 15u << 20; word32(after, 0x960, 127u << 20); break;
    case 16: S.img[0][7] = 16; word32(after, 0x158, 16); break;
    case 17: S.tex[0][255] = 17; word32(after, 0x560, 127); break;
    default: abort();
    }
    ref second = get(st, bank, dyn);
    assert(!same(first, second));
    assert(used() == allocated + 0x1000 && !memcmp(bytes(second), after, sizeof after));
    assert(!memcmp(bytes(first), before, sizeof before)); /* replacement cannot overwrite an earlier draw's copy */
    assert(same(second, get(st, bank, dyn)) && used() == allocated + 0x1000);
    memset(&S, 0, sizeof S); memset(g_dispatch, 0, sizeof g_dispatch); g_draw_binst = 0;
    ref third = get(st, bank, dyn); assert(!same(third, second) && !same(third, first));
    assert(!memcmp(bytes(third), before, sizeof before) && !memcmp(bytes(second), after, sizeof after));
    assert(same(third, get(st, bank, dyn)) && used() == allocated + 0x2000);
}
static void banks(void) {
    clean(); ref zero = get(5, 0, 0); uint8_t expected[0x1000] = {0};
    assert(!memcmp(bytes(zero), expected, sizeof expected));
    /* Both hash prefixes share a cache: the only changed byte is past c0's 0x648 limit. */
    S.samp[5][127] = 3u << 20; word64(expected, 0x960, 3u << 20);
    ref c2 = get(5, 2, 0); assert(!same(zero, c2));
    assert(!memcmp(bytes(c2), expected, sizeof expected));
    /* The short bank cannot ignore a cached long bank's nonzero tail. */
    ref c0 = get(5, 0, 0); assert(!same(c2, c0));
    S.samp[5][127] = 0;
    assert(same(c0, get(5, 2, 0))); // identical zero contents can share across banks
    S.samp[5][127] = 3u << 20; c2 = get(5, 2, 0);
    /* Different stages can share when their actual bytes match. */
    S.samp[1][127] = 3u << 20; VkDeviceSize n = used();
    assert(same(c2, get(1, 2, 0)) && used() == n);
}
static void epochs(void) {
    clean(); ref first = get(5, 0, 0); VkDeviceSize n = used();
    forbidden_compare = bytes(first); /* comparator must never touch a prior epoch's pointer */
    g_set_epoch++; ref next = get(5, 0, 0);
    assert(!same(first, next) && used() == n + 0x1000 && comparisons == 0);
    forbidden_compare = NULL;
    /* Rewinding an idle slot may reuse the same address, but still requires a new copy. */
    g_set_epoch++; for (int i = 0; i < slot.nblk; i++) slot.blk[i].used = 0;
    memset(bytes(next), 0xa5, 0x1000); forbidden_compare = bytes(next);
    ref recycled = get(5, 0, 0); uint8_t expected[0x1000] = {0};
    assert(used() == 0x1000 && !memcmp(bytes(recycled), expected, sizeof expected) && comparisons == 0);
    forbidden_compare = NULL;
}
static void disabled(void) {
    clean(); ref first = get(0, 0, 0); VkDeviceSize n = used();
    forbidden_compare = bytes(first); g_x_nodrvc = 1;
    ref next = get(0, 0, 0); assert(!same(first, next) && used() == n + 0x1000 && comparisons == 0);
    forbidden_compare = bytes(next); ref last = get(0, 0, 0);
    assert(!same(last, next) && used() == n + 0x2000 && comparisons == 0);
    forbidden_compare = NULL; g_x_nodrvc = 0;
    assert(same(last, get(0, 0, 0)) && used() == n + 0x2000);
}
static void growth(void) {
    clean(); g_draw_binst = 123; ref first = get(0, 0, 0);
    uint8_t expected[0x1000] = {0}; word32(expected, 0x610, 0x44ffe000); word32(expected, 0x644, 123);
    uintptr_t old_metadata = (uintptr_t)slot.blk; forced_hash = 8;
    /* One large unrelated scratch allocation per iteration fills a block, then grows metadata. */
    for (int i = 0; i < 10; i++) { VkBuffer b; VkDeviceSize o; uint8_t *p = vk_scratch(SCRATCH_BLOCK, 256, &b, &o); memset(p, i + 1, SCRATCH_BLOCK); }
    assert((uintptr_t)slot.blk != old_metadata && !memcmp(bytes(first), expected, sizeof expected));
    forced_hash = 7; VkDeviceSize n = used();
    assert(same(first, get(0, 0, 0)) && used() == n);
    /* Ordinary replacement leaves both allocations intact after metadata growth. */
    g_draw_binst = 124; ref second = get(0, 0, 0);
    assert(!same(first, second) && !memcmp(bytes(first), expected, sizeof expected));
    word32(expected, 0x644, 124); assert(!memcmp(bytes(second), expected, sizeof expected));
}
static void live_range(void) {
    for (int bank=0;bank<=2;bank+=2) {
        clean(); ref first = get(5,bank,0); VkDeviceSize n=used();
        expected_compare=bank ? 0x968 : 0x648;
        assert(same(first,get(5,bank,0)) && used()==n && comparisons==1);
    }
}
static void ordinary(void) {
    clean(); force_hash = 0; g_draw_binst = 77;
    ref first = get(0, 0, 0); VkDeviceSize n = used();
    assert(same(first, get(0, 0, 0)) && used() == n);
    g_draw_binst = 78; ref next = get(0, 0, 0);
    uint32_t value; memcpy(&value, bytes(next) + 0x644, sizeof value);
    assert(!same(first, next) && value == 78 && used() == n + 0x1000);
}
int main(int argc, char **argv) {
    (void)compare_bytes; assert(argc == 2); V.props.limits.minUniformBufferOffsetAlignment = 256;
    switch (atoi(argv[1])) {
    case 0: for (unsigned i = 0; i < 3; i++) changed(i); break;
    case 1: for (unsigned i = 3; i < 10; i++) changed(i); break;
    case 2: for (unsigned i = 10; i < 14; i++) changed(i); break;
    case 3: for (unsigned i = 14; i < 18; i++) changed(i); break;
    case 4: banks(); break;
    case 5: epochs(); break;
    case 6: disabled(); break;
    case 7: growth(); break;
    case 8: ordinary(); break;
    case 9: live_range(); break;
    default: abort();
    }
    clean(); return 0;
}
'''


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class DriverCache(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='vk-driver-cache-')
        cls.addClassCleanup(cls.temp.cleanup)
        cfile = Path(cls.temp.name) / 'driver.c'
        cls.exe = Path(cls.temp.name) / 'driver'
        cfile.write_text(harness())
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
                        '-I' + str(DM / 'third_party/vulkan-headers/include'),
                        str(cfile), '-o', str(cls.exe)], check=True)

    def check_case(self, case):
        result = subprocess.run([str(self.exe), str(case)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_graphics_collision_fields(self):
        self.check_case(0)

    def test_compute_collision_fields(self):
        self.check_case(1)

    def test_handle_collision_fields(self):
        self.check_case(2)

    def test_dynamic_slot_collision_fields(self):
        self.check_case(3)

    def test_cross_bank_and_stage_identity(self):
        self.check_case(4)

    def test_epoch_precedes_pointer_access(self):
        self.check_case(5)

    def test_disabled_cache_skips_pointer_access(self):
        self.check_case(6)

    def test_scratch_growth_keeps_prior_copies(self):
        self.check_case(7)

    def test_hits_compare_only_live_cpu_bytes(self):
        self.check_case(9)

    def test_normal_hash_reuse(self):
        self.check_case(8)


if __name__ == '__main__':
    unittest.main()
