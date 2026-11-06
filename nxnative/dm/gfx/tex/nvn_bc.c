#include "nvn_bc.h"
#include <limits.h>
#include <string.h>

#define BCDEC_STATIC
#define BCDEC_BC4BC5_PRECISE
#define BCDEC_IMPLEMENTATION
#include "../../third_party/bcdec/bcdec.h"

unsigned nvn_bc_pixel_size(int f) {
    if (f < 0x41 || f > 0x50) return 0;
    return f == 0x49 || f == 0x4a ? 1 : f == 0x4b || f == 0x4c ? 2 : f >= 0x4f ? 8 : 4;
}
unsigned nvn_bc_block_size(int f) {
    if (!nvn_bc_pixel_size(f)) return 0;
    return f == 0x41 || f == 0x42 || f == 0x45 || f == 0x46 || f == 0x49 || f == 0x4a ? 8 : 16;
}
// Validate with divisions before any pitch multiplication or pointer arithmetic.
static int fits(size_t size, size_t row, size_t slice, size_t bytes, unsigned rows, unsigned depth) {
    if (row < bytes || !rows || !depth || row > SIZE_MAX / rows || slice < row * rows) return 0;
    return slice <= SIZE_MAX / depth && size >= (size_t)(depth - 1) * slice + (size_t)(rows - 1) * row + bytes;
}
int nvn_bc_decode(int f, unsigned w, unsigned h, unsigned d,
                  const void *src, size_t src_size, size_t src_row, size_t src_slice,
                  void *dst, size_t dst_size, size_t dst_row, size_t dst_slice) {
    unsigned pixel = nvn_bc_pixel_size(f), block = nvn_bc_block_size(f);
    if (!pixel || !src || !dst || !w || !h || !d || w > UINT_MAX - 3 || h > UINT_MAX - 3) return -1;
    unsigned nx = (w + 3) / 4, ny = (h + 3) / 4;
    if ((size_t)nx > SIZE_MAX / block || (size_t)w > SIZE_MAX / pixel ||
        !fits(src_size, src_row, src_slice, (size_t)nx * block, ny, d) ||
        !fits(dst_size, dst_row, dst_slice, (size_t)w * pixel, h, d)) return -1;
    for (unsigned z = 0; z < d; z++) for (unsigned y = 0; y < ny; y++) for (unsigned x = 0; x < nx; x++) {
        // bcdec uses word loads: copy blocks into aligned local storage, including
        // unaligned subregions of guest copy buffers. Half output is aligned too.
        uint64_t in[2] = {0}, out[16] = {0};
        memcpy(in, (const uint8_t *)src + (size_t)z * src_slice + (size_t)y * src_row + (size_t)x * block, block);
        switch (f) {
        case 0x41: case 0x42: case 0x45: case 0x46: bcdec_bc1(in, out, 16); break;
        case 0x43: case 0x47: bcdec_bc2(in, out, 16); break;
        case 0x44: case 0x48: bcdec_bc3(in, out, 16); break;
        case 0x49: case 0x4a: bcdec_bc4(in, out, 4, f == 0x4a); break;
        case 0x4b: case 0x4c: bcdec_bc5(in, out, 8, f == 0x4c); break;
        case 0x4d: case 0x4e: bcdec_bc7(in, out, 16); break;
        case 0x4f: case 0x50: {
            uint16_t rgb[48]; bcdec_bc6h_half(in, rgb, 12, f == 0x4f);
            uint16_t *rgba = (uint16_t *)out;
            for (unsigned i = 0; i < 16; i++) {
                memcpy(rgba + 4 * i, rgb + 3 * i, 6); rgba[4 * i + 3] = 0x3c00; // alpha 1
            }
            break;
        }
        }
        if (f == 0x41 || f == 0x45) for (unsigned i = 0; i < 16; i++) ((uint8_t *)out)[4 * i + 3] = 255;
        unsigned cols = w - x * 4 < 4 ? w - x * 4 : 4, rows = h - y * 4 < 4 ? h - y * 4 : 4;
        for (unsigned r = 0; r < rows; r++)
            memcpy((uint8_t *)dst + (size_t)z * dst_slice + (size_t)(y * 4 + r) * dst_row + (size_t)x * 4 * pixel,
                   (uint8_t *)out + (size_t)r * 4 * pixel, (size_t)cols * pixel);
    }
    return 0;
}
