// Linux port: Android's AHardwareBuffer does not exist here. The Vulkan backend (dm/android/gfx_vk_*.c) treats a failed allocation as "no zero copy" and
// uses its copying path, so these stubs only have to compile and fail. (To do: dma-buf from Mesa, mapped over the guest's pages, in place of them.)
#pragma once
#include <stdint.h>
typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct AHardwareBuffer_Desc { uint32_t width, height, layers, format; uint64_t usage; uint32_t stride, rfu0; uint64_t rfu1; } AHardwareBuffer_Desc;
enum { AHARDWAREBUFFER_FORMAT_BLOB = 0x21 };
enum { AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN = 3, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN = 0x30, AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER = 1ull << 24,
       AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE = 0x100, AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT = 0x200 };
static inline int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *d, AHardwareBuffer **o) { (void)d; *o = 0; return -1; }
static inline void AHardwareBuffer_release(AHardwareBuffer *b) { (void)b; }
static inline int AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer *b, int fd) { (void)b; (void)fd; return -1; }
static inline void AHardwareBuffer_describe(const AHardwareBuffer *b, AHardwareBuffer_Desc *d) { (void)b; (void)d; }
