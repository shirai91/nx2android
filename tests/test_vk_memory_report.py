"""Driver memory-event accounting without a Vulkan device."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from porter.toolchain import find_ndk


class MemoryReport(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_event_lifetimes_and_options(self):
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        src = (ROOT / 'nxnative/dm/android/gfx_vk_core.c').read_text()
        helper = src[src.index('// DM_VK_MEMORY_REPORT=1:'):src.index('static void completion_start(void);')]
        code = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include "nvn_hmap.h"
static struct { VkPhysicalDevice pd; VkDevice dev; } V;
static VkBool32 feature; static unsigned queried, logged;
static void report_log(const char *fmt, ...) {
    (void)fmt; logged++;
}
#define VKLOG report_log
void vkGetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *q) {
    (void)pd; queried++;
    VkPhysicalDeviceDeviceMemoryReportFeaturesEXT *f = q->pNext;
    assert(f->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_MEMORY_REPORT_FEATURES_EXT && f->pNext == NULL);
    f->deviceMemoryReport = feature;
}
''' + helper + r'''
static void event(VkDeviceMemoryReportEventTypeEXT kind, uint64_t id, uint64_t bytes, VkObjectType type) {
    VkDeviceMemoryReportCallbackDataEXT d = {.sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_REPORT_CALLBACK_DATA_EXT,
        .type = kind, .memoryObjectId = id, .size = bytes, .objectType = type, .objectHandle = UINT64_MAX, .heapIndex = UINT32_MAX};
    memory_report_event(&d, NULL);
}
static mr_type *type_stats(VkObjectType type) {
    for (unsigned i = 0; i < g_mr_stats.ntypes; i++) if (g_mr_stats.types[i].type == type) return &g_mr_stats.types[i];
    assert(0); return NULL;
}
static void release(VkDeviceMemoryReportEventTypeEXT kind, uint64_t id) {
    event(kind, id, UINT64_MAX, VK_OBJECT_TYPE_MAX_ENUM);   // size and attribution fields are not used to retire saved metadata
}
static void *worker(void *arg) {
    uint64_t first = (uint64_t)(uintptr_t)arg << 32;
    for (unsigned i = 0; i < 2000; i++) {
        event(VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATE_EXT, first + i, 64, VK_OBJECT_TYPE_COMMAND_BUFFER);
        release(VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_FREE_EXT, first + i);
    }
    return NULL;
}
int main(void) {
    assert(!memory_report_prepare(0, 1) && queried == 0 && !atomic_load(&g_mr_on));
    event(VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATE_EXT, 7, 100, VK_OBJECT_TYPE_DEVICE_MEMORY);
    assert(g_mr_map.n == 0 && g_mr_stats.bytes[0] == 0);
    assert(!memory_report_prepare(1, 0) && queried == 0);
    assert(!memory_report_prepare(1, 1) && queried == 1 && !atomic_load(&g_mr_on));
    feature = VK_TRUE; assert(memory_report_prepare(1, 1) && queried == 2);
    assert(g_mr_create.pfnUserCallback == memory_report_event && V.dev == VK_NULL_HANDLE);
    unsigned before = logged;

    event(VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATE_EXT, 0, 100, VK_OBJECT_TYPE_PIPELINE);
    event(VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATE_EXT, 1, 200, VK_OBJECT_TYPE_IMAGE);
    assert(g_mr_map.n == 2 && g_mr_stats.bytes[0] == 300 && g_mr_stats.peak == 300);
    assert(type_stats(VK_OBJECT_TYPE_PIPELINE)->bytes[0] == 100);
    release(VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_FREE_EXT, 0);
    assert(type_stats(VK_OBJECT_TYPE_PIPELINE)->bytes[0] == 0 && type_stats(VK_OBJECT_TYPE_PIPELINE)->peak[0] == 100);
    release(VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_FREE_EXT, 1);
    assert(!g_mr_map.n && !g_mr_stats.bytes[0]);

    event(VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_IMPORT_EXT, 7, 400, VK_OBJECT_TYPE_DEVICE_MEMORY);
    event(VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_IMPORT_EXT, 7, 400, VK_OBJECT_TYPE_DEVICE_MEMORY);
    assert(g_mr_map.n == 1 && g_mr_stats.bytes[1] == 400 && type_stats(VK_OBJECT_TYPE_DEVICE_MEMORY)->objects[1] == 1);
    release(VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_UNIMPORT_EXT, 7);
    assert(g_mr_map.n == 1 && g_mr_stats.bytes[1] == 400);
    release(VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_UNIMPORT_EXT, 7);
    assert(!g_mr_map.n && !g_mr_stats.bytes[1]);

    // One physical ID may have owned and imported references; keep it counted once and transfer its attribution when ownership ends.
    event(VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATE_EXT, 8, 512, VK_OBJECT_TYPE_IMAGE);
    event(VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_IMPORT_EXT, 8, 512, VK_OBJECT_TYPE_DEVICE_MEMORY);
    assert(g_mr_stats.bytes[0] == 512 && g_mr_stats.bytes[1] == 0);
    release(VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_FREE_EXT, 8);
    assert(g_mr_stats.bytes[0] == 0 && g_mr_stats.bytes[1] == 512 && type_stats(VK_OBJECT_TYPE_DEVICE_MEMORY)->bytes[1] == 512);
    release(VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_UNIMPORT_EXT, 8);
    release(VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_FREE_EXT, 999);
    assert(!g_mr_map.n && g_mr_stats.unmatched == 1);
    event(VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATION_FAILED_EXT, UINT64_MAX, 8192, VK_OBJECT_TYPE_PIPELINE);
    assert(g_mr_stats.failed == 1 && g_mr_stats.failed_bytes == 8192 && !g_mr_map.n);
    assert(logged == before);

    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, worker, (void *)(i + 1)));
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    assert(!g_mr_map.n && !g_mr_stats.bytes[0] && !g_mr_stats.bytes[1] && !g_mr_stats.dropped);

    // Capacity exhaustion is observable without allocating or aborting inside the callback; deleting restores reusable capacity.
    for (uint64_t i = 0; i < MR_RECORDS; i++) event(VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATE_EXT, i, 1, VK_OBJECT_TYPE_BUFFER);
    assert(g_mr_map.n == MR_RECORDS && g_mr_map.cap == MR_CAP);
    event(VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATE_EXT, MR_RECORDS, 1, VK_OBJECT_TYPE_BUFFER);
    assert(g_mr_stats.dropped == 1 && g_mr_map.n == MR_RECORDS);
    for (uint64_t i = 0; i < MR_RECORDS; i++) release(VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_FREE_EXT, i);
    assert(!g_mr_map.n && !g_mr_stats.bytes[0] && !g_mr_stats.bytes[1]);
    assert(logged == before);
    memory_report_poll(); assert(logged > before); before = logged;
    memory_report_poll(); assert(logged == before);
    setenv("DM_VK_MEMORY_DUMP", "1", 1); memory_report_poll();
    assert(logged > before && !strcmp(getenv("DM_VK_MEMORY_DUMP"), "0"));
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='vk-memory-report-test-') as temp:
            cfile, exe = Path(temp) / 'report.c', Path(temp) / 'report-test'
            cfile.write_text(code)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-missing-field-initializers', '-idirafter', str(headers),
                            '-I' + str(ROOT / 'nxnative/dm/gfx/nvn'), '-pthread', str(cfile), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
