"""DM_VK_SLOT0_FILL (gfx_vk_exec.c): which GPUs and passes get a scratch target in an empty colour slot 0, and the render pass's store op for it, cut from the source."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from porter.toolchain import find_ndk
sys.path.insert(0, str(Path(__file__).resolve().parent))
import host_cc   # noqa: E402

EXEC = Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_exec.c'
HARNESS = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_hmap.h"
static char last[512];
#define VKLOG(...) snprintf(last, sizeof last, __VA_ARGS__)
#define VKRED_ONCE(id, ...) VKLOG(__VA_ARGS__)
static struct { VkDevice dev; VkPhysicalDeviceProperties props; } V;
static VkAttachmentDescription2 made[9]; static uint32_t nmade;
static VkResult create_rp(VkDevice d, const VkRenderPassCreateInfo2 *ci, const VkAllocationCallbacks *a, VkRenderPass *rp) {
    (void)d; (void)a; nmade = ci->attachmentCount; memcpy(made, ci->pAttachments, sizeof *made * nmade); *rp = (VkRenderPass)(uintptr_t)(0x100 + nmade); return VK_SUCCESS; }
static PFN_vkCreateRenderPass2 vkCreateRenderPass2 = create_rp;
RPKEY
static nvn_hmap g_rps = {.klen = sizeof(rpkey)}; static pthread_mutex_t g_rp_mu = PTHREAD_MUTEX_INITIALIZER;
CUTS
static int wanted(const char *name, uint32_t vendor, const char *env, int ncol, int view, uint32_t samples, uint32_t layers) {
    snprintf(V.props.deviceName, sizeof V.props.deviceName, "%s", name); V.props.vendorID = vendor;
    if (env) setenv("DM_VK_SLOT0_FILL", env, 1); else unsetenv("DM_VK_SLOT0_FILL");
    return slot0_fill_wanted(ncol, view, samples, layers);
}
int main(int argc, char **argv) {
    (void)argc; int mode = atoi(argv[1]);
    VkPhysicalDeviceProperties p = {0}; p.vendorID = 0x5143;
    snprintf(p.deviceName, sizeof p.deviceName, "Adreno (TM) 830"); assert(adreno_model(&p) == 830);
    snprintf(p.deviceName, sizeof p.deviceName, "Turnip Adreno (TM) 830"); assert(adreno_model(&p) == 830);
    snprintf(p.deviceName, sizeof p.deviceName, "Adreno (TM) 750"); assert(adreno_model(&p) == 750);
    snprintf(p.deviceName, sizeof p.deviceName, "Mali-G715"); assert(adreno_model(&p) == 0);
    p.vendorID = 0x13B5; snprintf(p.deviceName, sizeof p.deviceName, "Adreno (TM) 830"); assert(adreno_model(&p) == 0);
    // the switch is read once a run: each mode is a run of its own
    if (mode == 0) {   // an Adreno 830 by default: multi-target, single-sample, single-layer passes with slot 0 empty only
        assert(wanted("Adreno (TM) 830", 0x5143, NULL, 6, 0, 1, 1) && strstr(last, "a scratch target there") && strstr(last, "default"));
        assert(!slot0_fill_wanted(1, 0, 1, 1) && !slot0_fill_wanted(6, 1, 1, 1) && !slot0_fill_wanted(6, 0, 4, 1) && !slot0_fill_wanted(6, 0, 1, 6));
    } else if (mode == 1) assert(!wanted("Adreno (TM) 750", 0x5143, NULL, 6, 0, 1, 1) && strstr(last, "left empty"));   // an Adreno 7xx: off
    else if (mode == 2) assert(!wanted("Adreno (TM) 830", 0x5143, "0", 6, 0, 1, 1));   // switched off
    else if (mode == 3) assert(wanted("Mali-G715", 0x13B5, "1", 2, 0, 1, 1));           // forced on elsewhere
    else {   // the render pass: slot 0's scratch is not stored; the others are, as before (cstore 0 from memset)
        rpkey k; memset(&k, 0, sizeof k); k.ncol = 3; k.samples = 1;
        k.cfmt[0] = VK_FORMAT_R8G8B8A8_UNORM; k.cload[0] = VK_ATTACHMENT_LOAD_OP_DONT_CARE; k.cstore[0] = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        k.cfmt[2] = VK_FORMAT_R16G16B16A16_SFLOAT; k.cload[2] = VK_ATTACHMENT_LOAD_OP_LOAD;
        assert(rp_get(&k) && nmade == 2 && made[0].storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE && made[0].loadOp == VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        assert(made[1].format == VK_FORMAT_R16G16B16A16_SFLOAT && made[1].storeOp == VK_ATTACHMENT_STORE_OP_STORE);
    }
    puts("slot0 ok"); return 0;
}
'''


@unittest.skipUnless(shutil.which('cc') and host_cc.posix(), 'a POSIX host C compiler is required')
class Slot0Fill(unittest.TestCase):
    def test_gpus_passes_and_store_op(self):
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        src = EXEC.read_text()
        rpkey = next(line for line in src.splitlines() if '} rpkey;' in line)
        def cut(start, end_marker):
            a = src.index(start); return src[a:src.index(end_marker, a)]
        cuts = ('static int fmt_has_depth' + cut('static int fmt_has_depth', 'static VkRenderPass rp_compat(')[len('static int fmt_has_depth'):]
                + cut('static int adreno_model(', 'static int slot0_scratch('))
        with tempfile.TemporaryDirectory(prefix='nx2a-slot0-') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(HARNESS.replace('RPKEY', rpkey).replace('CUTS', cuts))
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-missing-field-initializers', '-pthread',
                            '-I', str(EXEC.parents[1] / 'gfx/nvn'), '-idirafter', str(headers), str(c), '-o', str(exe)], check=True)
            for mode in range(5):
                with self.subTest(mode=mode):
                    env = {k: v for k, v in os.environ.items() if k != 'DM_VK_SLOT0_FILL'}
                    out = subprocess.run([str(exe), str(mode)], capture_output=True, text=True, env=env, timeout=60)
                    self.assertEqual(out.returncode, 0, out.stderr)
                    self.assertIn('slot0 ok', out.stdout)


if __name__ == '__main__':
    unittest.main()
