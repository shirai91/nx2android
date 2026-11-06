// CPU BC decoding. Input is linear blocks after deswizzling, not Tegra storage.
#pragma once
#include <stddef.h>
#include <stdint.h>

// NVN formats 0x41..0x50: native R8/RG8 (signed where appropriate), RGBA8,
// or RGBA16F for BC6H. sRGB bytes are preserved; the image view does the conversion.
unsigned nvn_bc_pixel_size(int format);
unsigned nvn_bc_block_size(int format);
// Pitches are bytes; slices contain ceil(height/4) block rows. Returns -1 for
// invalid formats, dimensions, pitches or truncated buffers. Clips edge blocks.
int nvn_bc_decode(int format, unsigned width, unsigned height, unsigned depth,
                  const void *src, size_t src_size, size_t src_row, size_t src_slice,
                  void *dst, size_t dst_size, size_t dst_row, size_t dst_slice);
