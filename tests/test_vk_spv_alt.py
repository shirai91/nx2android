"""Exercise shader override selection and pipeline keys without a Vulkan device."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'nxnative/dm/android/gfx_vk_exec.c'


class ShaderOverride(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_targeted_pipeline_keys(self):
        src = SOURCE.read_text(encoding='utf-8')
        options = src[src.index('static uint64_t spv_key_read('):src.index('static void x_read(')]
        graphics_types = src[src.index('typedef struct { uint32_t raster_discard,'):src.index('static nvn_hmap g_pipes')]
        group_alt = src[src.index('r->alt = g_spv_key'):].split(';', 1)[0] + ';'
        select_start = src.index('    if (g_spv_key) {', src.index('static int prepare_draw('))
        select = src[select_start:src.index('    if (g_tracing) {', select_start)]
        variant_start = src.index('        uint32_t alt = spv_stage_alt(')
        variant = src[variant_start:src.index('        int fd =', variant_start)]
        compute_maps = src[src.index('static nvn_hmap g_cpipes'):src.index('static pthread_mutex_t g_cpipe_mu')]
        compute_start = src.index('    int alt = spv_compute_alt(g_spv_alt')
        compute = src[compute_start:src.index('    pthread_mutex_lock(&g_cpipe_mu)', compute_start)]
        code = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_hmap.h"
''' + options + graphics_types + compute_maps + r'''
_Static_assert(sizeof(pkey) == 992 && offsetof(pkey, alt_stages) == 36, "recorded graphics key layout");
static int g_spv_alt;
static uint64_t g_spv_key;
static pk_rs g_rs;
static pk_vi g_vi;
static pk_at g_at;
static void read_options(int alt) {
    g_spv_alt = alt;
    g_spv_key = spv_key_read(alt);
    pk_rs *r = &g_rs;
''' + group_alt + r'''
}
static pkey graphics_key(const uint64_t input[3], int *full_path) {
    int full = *full_path;
    pkey k = {0};
    const pk_rs *rs = &g_rs;
    if (full) { k.rs = g_rs; k.vi = g_vi; k.at = g_at; rs = &k.rs; }
    uint64_t keys[3], um[3] = {0}, im[3] = {0};
    memcpy(keys, input, sizeof keys);
    uint32_t tp = 7, rst = 0, fdz = 0;
''' + select + r'''
    if (!full) k.rs = g_rs;
    *full_path = full;
    return k;
}
static uint32_t module_variant(const pkey *k, int i, const uint32_t xo[3]) {
''' + variant + r'''
    return variant;
}
typedef struct { nvn_hmap *cache; uint64_t words[3]; int alt; } compute_selection;
static compute_selection compute_key(uint64_t key, uint64_t g_num_um, uint64_t g_num_im) {
    struct { uint64_t key; } shader = {key}, *cs = &shader;
''' + compute + r'''
    return (compute_selection){cache, {ck[0], ck[1], ck[2]}, alt};
}
int main(void) {
    uint64_t keys[3] = {0x1234, 0x5678, 0xfedcba9876543210ull};
    const uint32_t xo[3] = {0, 0, 0};
    const uint32_t alt7 = 1u << 31 | 7;
    int full = 0;
    assert(unsetenv("DM_VK_SPV_KEY") == 0);
    read_options(7);
    pkey legacy = graphics_key(keys, &full);
    assert(!full && legacy.rs.alt == 7 && !legacy.alt_stages);
    for (int i = 0; i < 3; i++) assert(module_variant(&legacy, i, xo) == alt7);
    assert(setenv("DM_VK_SPV_KEY", "0", 1) == 0);
    read_options(7);
    assert(!g_spv_key && g_rs.alt == 7);

    assert(setenv("DM_VK_SPV_KEY", "0x1234", 1) == 0);
    read_options(7);
    g_vi.nattr = 4; g_at.ncol = 2;
    pkey vertex = graphics_key(keys, &full);
    assert(full && vertex.rs.alt == 7 && vertex.alt_stages == 1 && vertex.vi.nattr == 4 && vertex.at.ncol == 2);
    assert(module_variant(&vertex, 0, xo) == alt7 && !module_variant(&vertex, 1, xo) && !module_variant(&vertex, 2, xo));
    assert(setenv("DM_VK_SPV_KEY", "FEDCBA9876543210", 1) == 0);
    read_options(7);
    full = 0;
    pkey geometry = graphics_key(keys, &full);
    assert(full && geometry.alt_stages == 4 && memcmp(&vertex, &geometry, sizeof vertex));
    assert(!module_variant(&geometry, 0, xo) && module_variant(&geometry, 2, xo) == alt7);
    assert(module_variant(&vertex, 0, xo) == alt7);  // a queued key keeps its original selection
    geometry.im[2] = 1; assert(!module_variant(&geometry, 2, xo));
    geometry.im[2] = 0; assert(module_variant(&geometry, 2, (uint32_t[3]){0, 0, 16}) == 16);
    keys[2] = 0; full = 0;
    pkey absent = graphics_key(keys, &full);
    assert(!full && !absent.rs.alt && !absent.alt_stages);
    full = 1; absent = graphics_key(keys, &full);
    assert(full && !absent.rs.alt && !absent.alt_stages);

    assert(setenv("DM_VK_SPV_KEY", "5678", 1) == 0);
    read_options(7); g_rs.raster_discard = 1; full = 0;
    pkey discarded = graphics_key(keys, &full);
    assert(!full && !discarded.rs.alt && !discarded.alt_stages);
    g_rs.raster_discard = 0;
    pkey fragment = graphics_key(keys, &full);
    assert(full && fragment.alt_stages == 2 && module_variant(&fragment, 1, xo) == alt7);
    read_options(0); full = 0;
    pkey base = graphics_key(keys, &full);
    assert(!g_spv_key && !full && !base.rs.alt && !base.alt_stages);

    assert(setenv("DM_VK_SPV_KEY", "1234", 1) == 0);
    read_options(7);
    compute_selection a = compute_key(0x1234, 0, 0), b = compute_key(0x5678, 0, 0);
    assert(a.alt == 7 && a.cache == &g_caltpipes && a.words[2] == 7);
    assert(!b.alt && b.cache == &g_cpipes && !b.words[2]);
    uint64_t im = 1ull << 48;
    b = compute_key(0x1234, 0, im);
    assert(!b.alt && b.cache == &g_cpipes && b.words[2] == im);
    read_options(65537); a = compute_key(0x1234, 0, 0);
    assert(a.alt == 65537 && a.cache == &g_caltpipes && a.words[2] == 65537);
    read_options(0); b = compute_key(0x1234, 0, 0);
    assert(!b.alt && b.cache == &g_cpipes && !b.words[2]);
    assert(unsetenv("DM_VK_SPV_KEY") == 0);
    read_options(7); b = compute_key(0x5678, 0, 0);
    assert(b.alt == 7 && b.cache == &g_caltpipes && b.words[2] == 7);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a_spv_alt_') as tmp:
            cfile, exe = Path(tmp) / 'spv_alt.c', Path(tmp) / 'check'
            cfile.write_text(code, encoding='utf-8')
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'nxnative/dm/gfx/nvn'), str(cfile), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
