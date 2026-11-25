"""Exercise the actual graphics pipeline builder with reported vertex format support."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from csrc import REPO, between, function, unit

STUBS = r"""
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#ifdef _WIN32
#define setenv(n, v, o) _putenv_s(n, v)   /* MinGW has no setenv */
#endif

static struct {
    VkPhysicalDevice pd;
    VkDevice dev;
    VkPipelineCache pcache;
    int has_divisor, has_depth_clip_ctl, has_dynrender;
} V = { .has_dynrender = 1 };
typedef struct { VkFormat vk; } vkfmt_t;
static const vkfmt_t *vk_fmt(int f) {
    static const vkfmt_t r32 = {VK_FORMAT_R32_SFLOAT};
    return f == 0x0a ? &r32 : NULL;
}
typedef struct {
    uint32_t in_locs, out_locs, local[3];
    int xfb;
    uint64_t key;
    VkShaderModule gs_mod[2];
    VkShaderModule mod;
} vkshader_t;
static vkshader_t *stage_vs;
static struct { uint64_t pipes_built; } g_vkst;
static struct { int phase; char what[400]; } g_build[1];
static int build_begin(void) { return 0; }
static void build_end(int slot) { assert(slot == 0); g_build[slot].phase = 0; }
static vkshader_t *vk_shader_gs_topo(vkshader_t *s, VkPrimitiveTopology t) { (void)t; return s; }
static int vk_shader_gs_split_ready(vkshader_t *s) { (void)s; return 1; }
static uint64_t last_vi;
static int no_variant;   /* the vertex variant could not be made: vk_shader_module hands back the original module */
static VkShaderModule vk_shader_module(vkshader_t *s, uint64_t a, uint64_t b, uint32_t c, int d, uint64_t e) {
    (void)s; (void)a; (void)b; (void)c; (void)d; if (s && s == stage_vs) last_vi = e;
    if (no_variant && s && e) return s->mod;
    return (VkShaderModule)(uintptr_t)12;
}
static const void *vk_sg_pin(VkShaderStageFlagBits stage, const uint32_t *local) {
    (void)stage; (void)local; return NULL;
}
static const uint32_t flatdbg_frag_spv[] = {0};
static PFN_vkCreateShaderModule vkCreateShaderModule;
static int rp_on(void) { return 0; }
static VkRenderPass rp_compat(uint32_t n, const uint32_t *c, uint32_t d, uint32_t s) {
    (void)n; (void)c; (void)d; (void)s; return VK_NULL_HANDLE;
}
static void vk_pipeline_cache_changed(void) {}
#define VKLOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LOG(...) VKLOG(__VA_ARGS__)
#define VKRED_ONCE(id, ...) VKLOG(__VA_ARGS__)
#define VKCHK(call) assert((call) == VK_SUCCESS)

static VkFormat unsupported, forwarded;
static int driver_calls;
static void vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice pd, VkFormat format, VkFormatProperties *p) {
    (void)pd;
    *p = (VkFormatProperties){.optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
        .bufferFeatures = format == unsupported ? 0 : VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT};
}
static VkResult vkCreateGraphicsPipelines(VkDevice dev, VkPipelineCache cache, uint32_t n,
    const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *p) {
    (void)dev; (void)cache; (void)a;
    assert(n == 1 && g_build[0].phase == 2);
    const VkPipelineVertexInputStateCreateInfo *vi = ci->pVertexInputState;
    driver_calls++;
    forwarded = vi->pVertexAttributeDescriptions[vi->vertexAttributeDescriptionCount - 1].format;
    if (forwarded == unsupported) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    *p = (VkPipeline)(uintptr_t)13;
    return VK_SUCCESS;
}
"""

PROBE = r"""
int main(int argc, char **argv) {
    assert(argc == 2);
    const int test = atoi(argv[1]);
    pkey k = {.nattr = 2, .nbind = 1, .samples = 1, .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, .key = {1}};
    k.attr[0].fmt = VK_FORMAT_R32G32B32A32_SFLOAT;
    k.attr[1].loc = 1;
    k.attr[1].off = 16;
    k.bind[0].stride = 32;
    if (test == 0) {
        unsupported = VK_FORMAT_R16G16B16_SFLOAT;
        k.attr[1].fmt = vertex_format(0x1d, 0, 0);
        assert(k.attr[1].fmt == unsupported);
    } else if (test == 1 || test == 2) {
        unsupported = test == 1 ? VK_FORMAT_R32_UINT : VK_FORMAT_R32_SFLOAT;
        k.attr[1].fmt = vertex_format(0x0a, 1, 0);
        assert(k.attr[1].fmt == VK_FORMAT_R32_UINT);
    } else if (test == 3) {
        /* A key restored for prewarm already contains the final format. */
        unsupported = VK_FORMAT_R16G16B16_SFLOAT;
        k.attr[1].fmt = VK_FORMAT_R16G16B16_SFLOAT;
    } else if (test == 4) {
        unsupported = VK_FORMAT_R32_UINT;
        k.attr[1].fmt = vertex_format(0x1d, 0, 0);
        assert(k.attr[1].fmt == VK_FORMAT_R16G16B16_SFLOAT);
    } else {
        /* An unsupported scaled format read by a float input: skipped as before (5), or with DM_VK_SCALED_FETCH=1 fetched as UINT and converted (6), unless no vertex variant could be made (7): skipped too. */
        unsupported = VK_FORMAT_R8G8B8A8_USCALED;
        k.attr[1].fmt = VK_FORMAT_R8G8B8A8_USCALED;
        if (test >= 6) setenv("DM_VK_SCALED_FETCH", "1", 1);
        if (test == 7) no_variant = 1;
    }
    vkshader_t v = {0}, *stages[3] = {&v}; stage_vs = &v;
    VkPipeline result = build_pipeline(&k, VK_NULL_HANDLE, stages);
    if (test == 0) {
        // Different failed keys sharing a format stay rejected without repeating its warning.
        for (unsigned i = 0; i < 300; i++) {
            k.key[0]++;
            assert(build_pipeline(&k, VK_NULL_HANDLE, stages) == (VkPipeline)(uintptr_t)1);
        }
        unsupported = VK_FORMAT_R32_UINT;
        k.attr[1].fmt = unsupported;
        assert(build_pipeline(&k, VK_NULL_HANDLE, stages) == (VkPipeline)(uintptr_t)1);
        assert(build_pipeline(&k, VK_NULL_HANDLE, stages) == (VkPipeline)(uintptr_t)1);
    }
    printf("%d %d %d\n", driver_calls, (int)forwarded, result == (VkPipeline)(uintptr_t)1);
    return 0;
}
"""


@unittest.skipUnless(shutil.which("cc"), "native C compiler unavailable")
class VertexFormatSupport(unittest.TestCase):
    def test_final_formats_are_checked_before_graphics_pipeline_creation(self):
        source = unit("nxnative/dm/android/gfx_vk_exec.c")
        formats = between(source, "static VkFormat vertex_format(", "static VkPrimitiveTopology topo(")
        key = between(source, "typedef struct {\n    uint64_t key[3];", "static nvn_hmap g_pipes")
        builder = function(source, "static VkPipeline build_pipeline(const pkey *k, VkPipelineLayout layout, vkshader_t *const stg_in[3]) {")
        headers = REPO / "nxnative/dm/third_party/vulkan-headers/include"
        with tempfile.TemporaryDirectory() as folder:
            probe = Path(folder) / "probe.c"
            binary = Path(folder) / "probe"
            probe.write_text(STUBS + formats + key + builder + PROBE, encoding="utf-8")
            compile_result = subprocess.run(
                ["cc", "-std=gnu11", "-I", str(headers), str(probe), "-o", str(binary)],
                capture_output=True, text=True,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            for scenario, expected in (
                (0, "0 0 1"),  # The Xclipse log's R16G16B16_SFLOAT.
                (1, "0 0 1"),  # Supported guest format remapped to an unsupported integer format.
                (2, "1 98 0"),  # Unsupported guest format remapped to a supported integer format.
                (3, "0 0 1"),  # Recorded final format, bypassing vertex_format().
                (4, "1 90 0"),  # Supported R16G16B16_SFLOAT, unchanged.
                (5, "0 0 1"),  # Unsupported R8G8B8A8_USCALED without DM_VK_SCALED_FETCH: skipped as before.
                (6, "1 41 0"),  # With DM_VK_SCALED_FETCH=1: fetched as R8G8B8A8_UINT (41), the vertex shader converts it.
                (7, "0 0 1"),  # The same, but no vertex variant could be made: an integer fetch into a float input is undefined, so skipped.
            ):
                with self.subTest(scenario=scenario):
                    result = subprocess.run([str(binary), str(scenario)], capture_output=True, text=True, check=True)
                    self.assertEqual(result.stdout.strip(), expected, result.stderr)
                    if expected.startswith("0"):
                        self.assertIn("[RED]", result.stderr)
                        self.assertEqual(result.stderr.count("graphics pipeline skipped:"), 2 if scenario == 0 else 1)
