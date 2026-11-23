"""Compile the production pruning policy, live control and diagnostics without a GPU."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class MaskedColorAttachments(unittest.TestCase):
    def test_policy_transitions_and_diagnostics(self):
        source = csrc.unit(ROOT / 'nxnative/dm/android/gfx_vk_exec.c')
        helpers = csrc.between(source, 'static void color_alias_map(', '// Render passes.')
        functions = ''.join(csrc.function(source, head) for head in (
            'static void prune_color_config(void)',
            'static void prune_masked_color(void)',
            'static void pruned_color_commit(void)',
        ))
        code = r'''
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct {
    int pass_open, samples, ncol;
    uint32_t daspect, cfmt[8], pass_w, pass_h, dfmt;
    struct { uint32_t mask; } chmask;
    struct { int a2c; } ms;
} S;
typedef struct image { void *bc_of; } image;
static image images[8];
static struct { image *im; } P_att[8];
static VkRenderingAttachmentInfo P_ca[8];
static VkImageView P_alias_views[8];
static uint8_t P_alias[8];
static int g_x_prune_color = 1, g_sub_pass = 3, history, ends, logs;
static char last_log[256];
static const char *setting;
static char *read_setting(const char *key) {
    assert(!strcmp(key, "DM_VK_X_PRUNE_COLOR")); return (char *)setting;
}
#define getenv read_setting
static int pixel_history_busy(void) { return history; }
static void vk_end_pass(void);
static void log_prune(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(last_log, sizeof last_log, fmt, ap); va_end(ap); logs++;
}
#define VKLOG(...) log_prune(__VA_ARGS__)
''' + helpers + functions + r'''
static void vk_end_pass(void) { ends++; S.pass_open=0; g_pruned_color=0; }
static void setup(int n, unsigned absent) {
    memset(&S,0,sizeof S); memset(P_ca,0,sizeof P_ca);
    S.pass_open=2; S.samples=1; S.daspect=VK_IMAGE_ASPECT_DEPTH_BIT;
    S.ncol=n; S.pass_w=1280; S.pass_h=720; S.dfmt=VK_FORMAT_D32_SFLOAT;
    g_x_prune_color=1; g_pruned_color=0; history=ends=0;
    for (int i=0;i<8;i++) {
        images[i].bc_of=NULL; P_att[i].im=&images[i];
        P_ca[i].imageView=(absent&(1u<<i))?VK_NULL_HANDLE:(VkImageView)(uintptr_t)(i+1);
        P_ca[i].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
        S.cfmt[i]=P_ca[i].imageView?VK_FORMAT_R8G8B8A8_UNORM:VK_FORMAT_UNDEFINED;
        P_alias_views[i]=P_ca[i].imageView; P_alias[i]=(uint8_t)i;
    }
}
int main(void) {
    // All slot counts and hole patterns; original count, extent and depth survive.
    for (int n=0;n<=8;n++) for (unsigned absent=0;absent<(1u<<n);absent++) {
        setup(n,absent); prune_masked_color();
        unsigned expected=((1u<<n)-1)&~absent; assert(g_pruned_color==expected);
        assert(S.ncol==n && S.pass_w==1280 && S.pass_h==720 && S.dfmt==VK_FORMAT_D32_SFLOAT);
        for (int i=0;i<n;i++) if (expected&(1u<<i))
            assert(!P_ca[i].imageView && !S.cfmt[i] && !P_att[i].im);
        assert(!g_prune_passes); // only successfully committed native passes count
    }
    // Every exclusion applies to the whole pass, including a later bound slot.
    for (int slot=0;slot<4;slot++) for (int reason=0;reason<11;reason++) {
        setup(4,0);
        switch(reason) {
        case 0: g_x_prune_color=0; break;
        case 1: history=1; break;
        case 2: S.pass_open=1; break;
        case 3: S.chmask.mask=1; break;
        case 4: S.samples=4; break;
        case 5: S.ms.a2c=1; break;
        case 6: S.daspect=VK_IMAGE_ASPECT_STENCIL_BIT; break;
        case 7: P_ca[slot].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; break;
        case 8: P_ca[slot].resolveImageView=(VkImageView)(uintptr_t)99; break;
        case 9: P_att[slot].im=NULL; break;
        case 10: images[slot].bc_of=&images[slot]; break;
        }
        prune_masked_color(); assert(!g_pruned_color);
        for (int i=0;i<4;i++) assert(P_ca[i].imageView && S.cfmt[i]);
    }
    // The control is re-read, not cached permanently: off/on/unset all work.
    setting="0"; prune_color_config(); assert(!g_x_prune_color);
    setting="1"; prune_color_config(); assert(g_x_prune_color);
    setting="0"; prune_color_config(); assert(!g_x_prune_color);
    setting=NULL; prune_color_config(); assert(g_x_prune_color);
    // A no-op prune does not count or log; committed passes log only the first five.
    setup(0,0); prune_masked_color(); pruned_color_commit(); assert(!g_prune_passes && !logs);
    for (int i=0;i<20;i++) {
        setup(4,0); prune_masked_color(); S.pass_open=1; pruned_color_commit();
    }
    assert(g_prune_passes==20 && logs==5);
    assert(strstr(last_log,"1280x720") && strstr(last_log,"depth format 126") && strstr(last_log,"0xf"));
    setup(4,0); prune_masked_color(); S.pass_open=1;
    bind_chmask(0); assert(!ends && g_pruned_color==15);
    bind_chmask(1); assert(ends==1 && !g_pruned_color && g_prune_split_color==1);
    bind_chmask(15); assert(ends==1 && g_prune_split_color==1);
    setup(4,0); prune_masked_color(); S.pass_open=1;
    pruned_color_end(&g_prune_split_a2c); assert(ends==1 && !g_pruned_color && g_prune_split_a2c==1);
    // Dropping a pending pass (e.g. a failed commit) is not an early native split.
    setup(4,0); prune_masked_color(); pruned_color_end(&g_prune_split_clear);
    assert(ends==1 && !g_prune_split_clear);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as folder:
            src = Path(folder) / 'test.c'
            exe = Path(folder) / 'test'
            src.write_text(code, encoding='utf-8')
            result = subprocess.run([
                'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I',
                str(ROOT / 'nxnative/dm/third_party/vulkan-headers/include'),
                str(src), '-o', str(exe),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
