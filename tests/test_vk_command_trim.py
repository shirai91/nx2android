"""The background trim resets command pools only after completed submission work."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from porter.toolchain import find_ndk


class CommandTrim(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_idle_reset_order(self):
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        source = (ROOT / 'nxnative/dm/android/gfx_vk_core.c').read_text()
        start = source.index('void mtl_finish(void) {')
        end = source.index('\n}\n', source.index('void mtl_drain(void) {', start)) + 3
        code = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
enum { NSLOT = 3, MIB = 1 << 20 };
static struct { VkDevice dev; } V;
static struct { VkCommandPool pool; } g_slot[NSLOT];
typedef struct { VkObjectType type; uint64_t bytes[2]; } mr_type;
static struct { mr_type types[2]; unsigned ntypes; } g_mr_stats = {
    {{VK_OBJECT_TYPE_COMMAND_BUFFER, {3 * MIB, 0}}, {VK_OBJECT_TYPE_DESCRIPTOR_POOL, {8 * MIB, 0}}}, 2};
static _Atomic int g_mr_on = 1;
static pthread_mutex_t g_mr_mu = PTHREAD_MUTEX_INITIALIZER, g_sub_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_fence;
static unsigned waits, resets, saves;
static int trim_enabled, completed, wait_succeeds = 1, fail_pool;
static char last_log[256];
static void log_line(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(last_log, sizeof last_log, fmt, args);
    va_end(args);
}
#define VKLOG log_line
static int mtl_wait(uint64_t fence, uint64_t timeout) {
    assert(fence == g_fence && timeout == UINT64_MAX);
    if (trim_enabled) assert(pthread_mutex_trylock(&g_sub_lock) == EBUSY);
    waits++;
    return completed = wait_succeeds;
}
static void vk_pipeline_cache_save(void) {
    assert(pthread_mutex_trylock(&g_sub_lock) == 0);
    pthread_mutex_unlock(&g_sub_lock);
    saves++;
}
VkResult vkResetCommandPool(VkDevice dev, VkCommandPool pool, VkCommandPoolResetFlags flags) {
    assert(dev == V.dev && pool == g_slot[resets % NSLOT].pool);
    assert(completed && pthread_mutex_trylock(&g_sub_lock) == EBUSY);
    assert(flags == VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT);
    resets++;
    if ((uintptr_t)pool == (uintptr_t)fail_pool) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(pthread_mutex_trylock(&g_mr_mu) == 0);   // the driver's memory callback may run during reset
    g_mr_stats.types[0].bytes[0] -= MIB;
    pthread_mutex_unlock(&g_mr_mu);
    return VK_SUCCESS;
}
''' + source[start:end] + r'''
int main(void) {
    unsetenv("DM_VK_TRIM_COMMANDS");
    g_fence = 19;
    mtl_drain();
    assert(waits == 1 && resets == 0 && saves == 1);
    for (unsigned i = 0; i < NSLOT; i++) g_slot[i].pool = (VkCommandPool)(uintptr_t)(i + 1);
    setenv("DM_VK_TRIM_COMMANDS", "1", 1);
    trim_enabled = 1;
    g_fence = 0;
    mtl_drain();
    assert(waits == 1 && resets == 0 && saves == 2);
    g_fence = 20;
    wait_succeeds = 0;
    mtl_drain();
    assert(waits == 2 && resets == 0 && saves == 3 && strstr(last_log, "timed out"));
    wait_succeeds = 1;
    mtl_drain();
    assert(waits == 3 && resets == NSLOT && saves == 4);
    assert(strstr(last_log, "reset 3/3") && strstr(last_log, "3.0 to 0.0 MiB"));
    g_mr_stats.types[0].bytes[0] = 3 * MIB;
    fail_pool = 2;
    mtl_drain();
    assert(waits == 4 && resets == 2 * NSLOT && saves == 5);
    assert(strstr(last_log, "reset 2/3") && strstr(last_log, "3.0 to 1.0 MiB"));
    assert(g_mr_stats.types[1].bytes[0] == 8 * MIB);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='vk-command-trim-test-') as temp:
            cfile, exe = Path(temp) / 'trim.c', Path(temp) / 'trim-test'
            cfile.write_text(code)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-idirafter', str(headers), '-pthread', str(cfile), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
