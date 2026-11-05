"""Guest mapping and resource ownership when importing a zero-copy pool."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from porter.toolchain import find_ndk


class PoolImport(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_import_before_replacing_guest_mapping(self):
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        src = (ROOT / 'nxnative/dm/android/gfx_vk_pool.c').read_text()
        start = src.index('static int pool_zero_copy(')
        helper = src[start:src.index('\n}\n', start) + 2]
        code = r'''
#define VK_USE_PLATFORM_ANDROID_KHR
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <vulkan/vulkan.h>
#ifndef __INTRODUCED_IN
#define __INTRODUCED_IN(api)
#endif
#ifndef __clang__
#define _Nonnull
#define _Nullable
#endif
#include <android/hardware_buffer.h>
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

typedef struct { VkBuffer buf; VkDeviceMemory mem; AHardwareBuffer *ahb; int zero_copy; uint64_t gpu; } vkpool_t;
static struct { VkDevice dev; } V;
enum Fault { SUCCESS, AHB_ALLOC, PROPERTIES, CREATE, VK_ALLOC, BIND, SOCKET, SEND, RECEIVE, TEMP_MAP, FIXED_MAP };
static enum Fault fault;
static unsigned ahbs, buffers, allocations, bound, fds, maps, temp_maps;
static uint8_t guest[4096], dmabuf[sizeof guest];
#define BUFFER ((VkBuffer)(uintptr_t)1)
#define BACKING ((VkDeviceMemory)(uintptr_t)2)
#define AHB ((AHardwareBuffer *)(uintptr_t)3)
#define POOL_USAGE_BDA VK_BUFFER_USAGE_STORAGE_BUFFER_BIT

static const void *pool_mem_flags(const void *p) { return p; }
static uint32_t vk_mem_type(uint32_t bits, VkMemoryPropertyFlags need, VkMemoryPropertyFlags avoid) {
    assert(bits == 1 && !need && !avoid); return 0;
}
static uint64_t buf_gpu(VkBuffer b) { assert(b == BUFFER && maps == 2); return 123; }
int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *desc, AHardwareBuffer **a) {
    assert(desc->width == sizeof guest);
    if (fault == AHB_ALLOC) return -1;
    *a = AHB; ahbs++; return 0;
}
void AHardwareBuffer_release(AHardwareBuffer *a) { assert(a == AHB && ahbs); ahbs--; }
VkResult vkGetAndroidHardwareBufferPropertiesANDROID(VkDevice d, const AHardwareBuffer *a,
                                                      VkAndroidHardwareBufferPropertiesANDROID *p) {
    (void)d; assert(a == AHB && ahbs);
    p->allocationSize = sizeof guest; p->memoryTypeBits = 1;
    return fault == PROPERTIES ? VK_ERROR_INVALID_EXTERNAL_HANDLE : VK_SUCCESS;
}
VkResult vkCreateBuffer(VkDevice d, const VkBufferCreateInfo *b, const VkAllocationCallbacks *c, VkBuffer *out) {
    (void)d; (void)c; assert(b->size == sizeof guest);
    *out = BUFFER;
    if (fault == CREATE) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    buffers++; return VK_SUCCESS;
}
VkResult vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo *a, const VkAllocationCallbacks *c, VkDeviceMemory *m) {
    (void)d; (void)c; assert(a->allocationSize == sizeof guest);
    const VkImportAndroidHardwareBufferInfoANDROID *imp = a->pNext;
    const VkMemoryDedicatedAllocateInfo *ded = imp->pNext;
    assert(imp->buffer == AHB && ded->buffer == BUFFER && ahbs == 1 && buffers == 1);
    *m = BACKING;
    if (fault == VK_ALLOC) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    allocations++; return VK_SUCCESS;
}
VkResult vkBindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize o) {
    (void)d; assert(b == BUFFER && m == BACKING && !o && allocations && buffers);
    if (fault == BIND) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    bound = 1; return VK_SUCCESS;
}
void vkDestroyBuffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks *c) {
    (void)d; (void)c; assert(b == BUFFER && buffers); buffers--;
}
void vkFreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *c) {
    (void)d; (void)c; assert(m == BACKING && allocations && !buffers); allocations--;
}
static int mock_socketpair(int domain, int type, int protocol, int sv[2]) {
    assert(domain == AF_UNIX && type == (SOCK_SEQPACKET | SOCK_CLOEXEC) && !protocol);
    if (fault == SOCKET) return -1;
    sv[0] = 3; sv[1] = 4; fds = (1u << 3) | (1u << 4);
    return 0;
}
int AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer *a, int sock) {
    assert(a == AHB && sock == 3 && (fds & (1u << 3)));
    return fault == SEND ? -1 : 0;
}
static int recv_fds(int sock, int *out, int max) {
    assert(sock == 4 && max >= 2 && (fds & (1u << 4)));
    if (fault == RECEIVE) return -1;
    out[0] = 5; out[1] = 6; fds |= (1u << 5) | (1u << 6);
    return 2;
}
static void mock_close(int fd) { assert(fds & (1u << fd)); fds &= ~(1u << fd); }
static void *mock_mmap(void *addr, size_t size, int prot, int flags, int fd, int off) {
    assert(size == sizeof guest && prot == (PROT_READ | PROT_WRITE) && fd == 5 && !off);
    assert(bound && (fds & (1u << 5)));
    maps++;
    if (!addr) {
        assert(flags == MAP_SHARED && !temp_maps);
        if (fault == TEMP_MAP) return MAP_FAILED;
        temp_maps++;
        return dmabuf;
    }
    assert(addr == guest && flags == (MAP_SHARED | MAP_FIXED));
    assert(!memcmp(dmabuf, guest, sizeof guest));
    return fault == FIXED_MAP ? MAP_FAILED : guest;
}
static void mock_munmap(void *addr, size_t size) {
    assert(addr == dmabuf && size == sizeof dmabuf && temp_maps == 1);
    temp_maps--;
}
#define socketpair mock_socketpair
#define close mock_close
#define mmap mock_mmap
#define munmap mock_munmap
''' + helper + r'''

int main(void) {
    for (size_t i = 0; i < sizeof guest; i++) guest[i] = (uint8_t)i;
    for (fault = SUCCESS; fault <= FIXED_MAP; fault++) {
        vkpool_t p = {0};
        bound = maps = 0;
        memset(dmabuf, 0, sizeof dmabuf);
        int r = pool_zero_copy(&p, guest, sizeof guest);
        if (fault == SUCCESS) {
            assert(!r && p.zero_copy && p.gpu == 123 && p.ahb == AHB && p.buf == BUFFER && p.mem == BACKING);
            assert(buffers == 1 && allocations == 1 && ahbs == 1);
            vkDestroyBuffer(V.dev, p.buf, NULL); vkFreeMemory(V.dev, p.mem, NULL); AHardwareBuffer_release(p.ahb);
        } else {
            assert(r == (fault >= PROPERTIES && fault <= BIND ? -2 : -1));
            assert(!p.zero_copy && !p.ahb && !p.gpu && !p.buf && !p.mem);
        }
        assert(maps == (fault == SUCCESS || fault == FIXED_MAP ? 2u : fault == TEMP_MAP ? 1u : 0u));
        assert(!buffers && !allocations && !ahbs && !fds && !temp_maps);
        for (size_t i = 0; i < sizeof guest; i++) assert(guest[i] == (uint8_t)i);
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='vk-import-test-') as temp:
            cfile, exe = Path(temp) / 'import.c', Path(temp) / 'import-test'
            cfile.write_text(code)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-missing-field-initializers', '-idirafter', str(headers),
                            str(cfile), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
