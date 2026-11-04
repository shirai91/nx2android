"""Check ordered BC buffer readback and command-buffer reuse with Vulkan stubbed."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm'


@unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
class BCSnapshot(unittest.TestCase):
    def test_ordered_readback_reuses_command_buffers(self):
        source = (DM / 'android/gfx_vk_core.c').read_text()
        start = source.index('void vk_buffer_snapshot(')
        helper = source[start:source.index('\nint vk_cmd_parts_left', start)]
        code = r'''
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAIN_CBS 32
#define VKCHK(call) assert((call) == VK_SUCCESS)
#define die(...) abort()
typedef struct { VkCommandBuffer main, up, mains[MAIN_CBS]; int nmain_sent, nmain_used, up_used, cb_err; } vksub_t;
static vksub_t sub;
static struct { VkDevice dev; VkQueue q; } V;
static pthread_mutex_t g_vk_queue_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_cmd_gen;
static uint8_t gpu[32], scratch[32]; static VkBufferCopy copy;
static unsigned submitted, waited, begun, reset, barrier;
static vksub_t *cur(void) { return &sub; }
static VkCommandBuffer vk_cmd(void) { return sub.main; }
static void vk_img_barrier_all(VkCommandBuffer cb) { assert(cb == sub.main || cb == sub.up); }
static void *vk_scratch(size_t n, size_t a, VkBuffer *b, VkDeviceSize *o) { assert(n == 8 && a == 16); memset(scratch, 0xab, sizeof scratch); *b=(VkBuffer)9; *o=0; return scratch; }
void vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *r) {
    assert(cb == sub.main && s == (VkBuffer)8 && d == (VkBuffer)9 && n == 1); copy=*r;
}
void vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags flags,
                         uint32_t n, const VkMemoryBarrier *b, uint32_t nb, const VkBufferMemoryBarrier *bb, uint32_t ni, const VkImageMemoryBarrier *ib) {
    (void)flags; (void)bb; (void)ib;
    assert(cb == sub.main && s == VK_PIPELINE_STAGE_TRANSFER_BIT && d == VK_PIPELINE_STAGE_HOST_BIT);
    assert(n == 1 && !nb && !ni && b->srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT && b->dstAccessMask == VK_ACCESS_HOST_READ_BIT); barrier++;
}
VkResult vkEndCommandBuffer(VkCommandBuffer cb) { assert(cb == sub.main || cb == sub.up); return VK_SUCCESS; }
VkResult vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f) {
    (void)q; (void)f; assert(n == 1 && barrier == submitted + 1);
    assert(s->commandBufferCount == (submitted ? 1u : 4u));
    if (!submitted) { assert(s->pCommandBuffers[0] == sub.up); assert(s->pCommandBuffers[1] == sub.mains[0]); assert(s->pCommandBuffers[2] == sub.mains[1]); }
    assert(s->pCommandBuffers[s->commandBufferCount-1] == sub.main); submitted++; return VK_SUCCESS;
}
VkResult vkQueueWaitIdle(VkQueue q) {
    (void)q; assert(submitted == waited + 1); memcpy(scratch+copy.dstOffset, gpu+copy.srcOffset, copy.size); waited++; return VK_SUCCESS;
}
VkResult vkResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags f) { (void)f; assert(waited == submitted && (cb == sub.main || cb == sub.up)); reset++; return VK_SUCCESS; }
VkResult vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *b) { assert(cb == sub.main && b->flags == VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT); begun++; return VK_SUCCESS; }
''' + helper + r'''
int main(void) {
    sub.main=(VkCommandBuffer)3; sub.up=(VkCommandBuffer)4; sub.up_used=1; sub.nmain_used=3;
    for (int i=0;i<3;i++) sub.mains[i]=(VkCommandBuffer)(uintptr_t)(i+1);
    for (unsigned n=0;n<100;n++) {
        memset(gpu, n, sizeof gpu); uint8_t out[8]; memset(out, 0, sizeof out);
        vk_buffer_snapshot((VkBuffer)8, 4, sizeof out, out);
        for(unsigned i=0;i<sizeof out;i++) assert(out[i] == n);
        assert(sub.nmain_sent == 2 && sub.nmain_used == 3 && !sub.up_used);
    }
    assert(submitted == 100 && waited == 100 && begun == 100 && reset == 101 && g_cmd_gen == 100);
    puts("ordered readback and 100 buffer reuses passed");
}
'''
        with tempfile.TemporaryDirectory() as folder:
            src = Path(folder) / 'snapshot.c'; src.write_text(code)
            exe = Path(folder) / 'snapshot'
            subprocess.run(['cc', '-std=c11', '-pthread', '-I', str(DM / 'third_party/vulkan-headers/include'),
                            str(src), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
            self.assertIn('100 buffer reuses passed', result.stdout)
