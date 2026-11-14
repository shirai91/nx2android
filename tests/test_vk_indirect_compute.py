"""Run the production replay with a delayed Vulkan command sink, without a device.

The sink executes buffer operations only after replay, so copying CPU argument
bytes cannot satisfy these tests. Full stage_set/dispatch and driver caching are
compiled unchanged; only unrelated texture/pipeline services and Vulkan calls
are supplied by the fixture.
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


def production():
    replay = (DM / 'android/gfx_vk_exec.c').read_text()
    core = (DM / 'android/gfx_vk_core.c').read_text()
    header = (DM / 'android/gfx_vk.h').read_text()
    # Real runtime structs and Vulkan declarations, without Android OS headers.
    header = '\n'.join(line for line in header.splitlines()
                       if not any(x in line for x in ('VK_USE_PLATFORM_ANDROID_KHR',
                           '#include <android/', '#include "dm.h"')))
    header = header.replace('<vulkan/vulkan.h>', '<vulkan/vulkan_core.h>')
    for name in ('vkCreateAndroidSurfaceKHR', 'vkGetAndroidHardwareBufferPropertiesANDROID'):
        header = header.replace('X(' + name + ')', '')
    state = replay[replay.index('static struct {'):replay.index('} S;') + 4]
    slots = core[core.index('#define SCRATCH_BLOCK'):core.index('static uint64_t pool_merge_back')]
    drvs = replay[replay.index('static uint32_t g_dispatch'):replay.index('// vk_samp_by_id without')]
    cache = replay[replay.index('enum { SETC'):replay.index('// A binding that runs past')]
    memo = replay[replay.index('enum { MEMO_CB'):replay.index('// ---------------- draws')]
    indirect = replay[replay.index('static void compute_warm_start(void) {'):replay.index('// ---------------- copies and clears')]
    indirect = indirect[indirect.index('\n}') + 2:]
    ubd = core[core.index('typedef struct { uint64_t lo, hi; VkBuffer sb;'):core.index('// the draw bisection')]
    infl = core[core.index('typedef struct { uint64_t lo, hi, fence; const uint8_t *data; uint8_t *own; }'):core.index('int vk_ubd_overlaps')]
    functions = ''.join(function(core, sig) for sig in [
        'static vkpool_t *pool_find(', 'vkpool_t *vk_pool_of(', 'uint32_t vk_pool_gen(', 'int vk_buf_at(',
        'static vksub_t *cur(', 'void *vk_scratch(', 'void vk_img_barrier_all('])
    # Single-line production functions need their line, not the next function.
    for sig in ['vkpool_t *vk_pool_of(', 'uint32_t vk_pool_gen(', 'static vksub_t *cur(']:
        functions = functions.replace(function(core, sig), next(x for x in core.splitlines() if x.startswith(sig)) + '\n')
    driver = function(replay, 'static uint64_t c2_handle(') + drvs + cache + memo + indirect
    lifecycle = function(core, 'static void slot_recycle(')
    end_pass = next(x for x in replay.splitlines() if x.startswith('void vk_end_pass_land(')) + '\n'
    wrapper = ('stage_set(st, s, offsets, NULL)' if 'uint32_t *dofs, VkDescriptorBufferInfo *' in replay
               else 'stage_set(st, s, offsets)')
    usage = core[core.index('static const VkBufferUsageFlags POOL_USAGE0'):core.index('static VkResult pool_copied_alloc')]
    return header, state, slots, functions + ubd + infl + end_pass, driver, lifecycle, wrapper, usage


@unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
class IndirectCompute(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='vk-indirect-compute-')
        cls.addClassCleanup(cls.tmp.cleanup)
        folder = Path(cls.tmp.name)
        header, state, slots, core, replay, lifecycle, wrapper, usage = production()
        (folder / 'runtime.h').write_text(header)
        code = (ROOT / 'tests/vk_indirect_compute_harness.c').read_text()
        for marker, value in [('STATE', state), ('SLOTS', slots), ('CORE', core),
                              ('REPLAY', replay), ('LIFECYCLE', lifecycle), ('STAGE_CALL', wrapper), ('USAGE', usage)]:
            code = code.replace('/* ' + marker + ' */', value)
        (folder / 'check.c').write_text(code)
        cls.exe = folder / 'check'
        subprocess.run(['cc', '-std=gnu11', '-pthread', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-unused-function',
                        '-Wno-unused-variable', '-Wno-missing-field-initializers', '-Wno-dangling-else',
                        '-I' + str(folder), '-I' + str(DM / 'third_party/vulkan-headers/include'),
                        '-I' + str(DM / 'gfx/nvn'), '-I' + str(DM / 'gfx/nvn/include'),
                        '-I' + str(DM / 'android'), str(folder / 'check.c'),
                        '-o', str(cls.exe)], check=True)

    def run_case(self, case):
        for dynamic in (0, 1):
            with self.subTest(dynamic=dynamic):
                subprocess.run([str(self.exe), case, str(dynamic)], check=True, timeout=15)

    def test_gpu_counts_and_cache_isolation(self):
        self.run_case('counts')

    def test_two_chunk_arguments_and_nonzero_buffer_offsets(self):
        self.run_case('two')

    def test_three_chunk_arguments_without_c0(self):
        self.run_case('three')

    def test_invalid_ranges_emit_no_partial_copy_or_dispatch(self):
        self.run_case('invalid')

    def test_deferred_uniform_update_precedes_argument_copy(self):
        self.run_case('deferred')

    def test_scratch_remains_live_until_submission_completion(self):
        self.run_case('lifetime')


if __name__ == '__main__':
    unittest.main()
