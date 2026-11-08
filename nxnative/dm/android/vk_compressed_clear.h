#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

// Constant blocks only, not a general texture compressor. Layouts follow the
// Khronos Data Format specification (S3TC, RGTC, BPTC and ASTC):
// https://registry.khronos.org/DataFormat/specs/1.4/dataformat.1.4.html
// Clear colors are linear; sRGB conversion affects RGB, never alpha.
static inline float vk_cc_clamp(float x, float low, float high) {
    if (isnan(x)) return 0.0f;
    return x < low ? low : x > high ? high : x;
}

static inline unsigned vk_cc_unorm(float x, unsigned maximum) {
    return (unsigned)(vk_cc_clamp(x, 0.0f, 1.0f) * maximum + 0.5f);
}

static inline void vk_cc_u16(uint8_t *dst, unsigned value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

// Bit numbering in BC6H/BC7 starts at the least significant bit of byte zero.
// The caller zeroes the block before writing fields; byte stores allow any
// destination alignment and do not depend on host endianness.
static inline void vk_cc_bits(uint8_t block[16], unsigned *bit, unsigned value, unsigned count) {
    for (unsigned i = 0; i < count; ++i, ++*bit)
        block[*bit / 8] |= (uint8_t)(((value >> i) & 1u) << (*bit % 8));
}

static inline void vk_cc_bc1(const float color[4], uint8_t block[8], int transparent) {
    unsigned rgb = (vk_cc_unorm(color[0], 31) << 11) |
                   (vk_cc_unorm(color[1], 63) << 5) | vk_cc_unorm(color[2], 31);
    vk_cc_u16(block, rgb);
    vk_cc_u16(block + 2, rgb);
    // Equal endpoints and index zero yield an opaque constant in BC1/2/3.
    // BC1 RGBA index three in this mode is transparent black.
    if (transparent) memset(block + 4, 0xff, 4);
}

static inline void vk_cc_bc4(float value, uint8_t block[8], int is_signed) {
    int endpoint = is_signed ? (int)roundf(vk_cc_clamp(value, -1.0f, 1.0f) * 127.0f)
                             : (int)vk_cc_unorm(value, 255);
    block[0] = block[1] = (uint8_t)endpoint;
}

// Decode the positive magnitude of a mode-11 endpoint. Search this monotonic
// mapping to choose the nearest representable float, including subnormals.
// BC6H first unquantizes to 16/15 bits, then scales by 31/64 or 31/32
// to obtain a finite binary16 bit pattern. See Microsoft's BC6H decoder:
// https://learn.microsoft.com/en-us/windows/win32/direct3d11/bc6h-format
static inline float vk_cc_bc6_value(unsigned q, int is_signed) {
    unsigned maximum = is_signed ? 511u : 1023u;
    unsigned half;
    if (!q) return 0.0f;
    if (q == maximum) half = 0x7bff;
    else {
        unsigned unquantized = is_signed ? ((q << 15) + 0x4000) >> 9
                                         : ((q << 16) + 0x8000) >> 10;
        half = (unquantized * 31) >> (is_signed ? 5 : 6);
    }
    unsigned exponent = half >> 10;
    return exponent ? ldexpf((float)(1024 + (half & 1023)), (int)exponent - 25)
                    : (float)half * 0x1p-24f;
}

static inline unsigned vk_cc_bc6_endpoint(float value, int is_signed) {
    value = vk_cc_clamp(value, is_signed ? -65504.0f : 0.0f, 65504.0f);
    int negative = value < 0.0f;
    float magnitude = fabsf(value);
    unsigned low = 0, high = is_signed ? 511u : 1023u;
    while (low < high) {
        unsigned mid = (low + high) / 2;
        if (vk_cc_bc6_value(mid, is_signed) < magnitude) low = mid + 1;
        else high = mid;
    }
    if (low && magnitude - vk_cc_bc6_value(low - 1, is_signed) <=
               vk_cc_bc6_value(low, is_signed) - magnitude) --low;
    // Signed endpoints are stored as 10-bit two's complement, not sign-magnitude.
    return negative ? (0u - low) & 1023u : low;
}

// Returns the encoded byte count (8 or 16), or zero without touching block for
// unsupported formats. Writes only that many bytes. NaN becomes zero; infinities
// clamp to the format's endpoints. BC1 RGBA rounds alpha to one bit (0.5 is opaque).
static inline int vk_compressed_clear_block(VkFormat format, const float color[4], uint8_t block[16]) {
    int astc = format >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
    if (!astc && (format < VK_FORMAT_BC1_RGB_UNORM_BLOCK || format > VK_FORMAT_BC7_SRGB_BLOCK)) return 0;
    int srgb = astc ? (format - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) & 1 :
               format == VK_FORMAT_BC1_RGB_SRGB_BLOCK || format == VK_FORMAT_BC1_RGBA_SRGB_BLOCK ||
               format == VK_FORMAT_BC2_SRGB_BLOCK || format == VK_FORMAT_BC3_SRGB_BLOCK ||
               format == VK_FORMAT_BC7_SRGB_BLOCK;
    float rgba[4];
    for (unsigned i = 0; i < 4; ++i) {
        rgba[i] = vk_cc_clamp(color[i], 0.0f, 1.0f);
        if (srgb && i < 3)
            rgba[i] = rgba[i] <= 0.0031308f ? 12.92f * rgba[i] :
                      1.055f * powf(rgba[i], 1.0f / 2.4f) - 0.055f;
    }
    int size = (format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK) ||
               format == VK_FORMAT_BC4_UNORM_BLOCK || format == VK_FORMAT_BC4_SNORM_BLOCK ? 8 : 16;
    memset(block, 0, (size_t)size);
    if (astc) {
        // LDR void-extent mode: reserved bits 10/11 are one, all four 13-bit
        // coordinates are all-one to disable the extent. The same block is
        // valid for every 2D footprint and can be copied to any mip/layer.
        memset(block, 0xff, 8);
        block[0] = 0xfc; block[1] = 0xfd;
        for (unsigned i = 0; i < 4; ++i) vk_cc_u16(block + 8 + 2 * i, vk_cc_unorm(rgba[i], 65535));
        return 16;
    }
    switch (format) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        vk_cc_bc1(rgba, block, (format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ||
                               format == VK_FORMAT_BC1_RGBA_SRGB_BLOCK) && rgba[3] < 0.5f);
        break;
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK: {
        unsigned alpha = vk_cc_unorm(rgba[3], 15);
        memset(block, (int)(alpha | (alpha << 4)), 8);
        vk_cc_bc1(rgba, block + 8, 0);
        break;
    }
    case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK:
        vk_cc_bc4(rgba[3], block, 0);
        vk_cc_bc1(rgba, block + 8, 0);
        break;
    case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC4_SNORM_BLOCK:
        vk_cc_bc4(color[0], block, format == VK_FORMAT_BC4_SNORM_BLOCK);
        break;
    case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_BC5_SNORM_BLOCK:
        vk_cc_bc4(color[0], block, format == VK_FORMAT_BC5_SNORM_BLOCK);
        vk_cc_bc4(color[1], block + 8, format == VK_FORMAT_BC5_SNORM_BLOCK);
        break;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK: {
        unsigned endpoint[3], bit = 0;
        for (unsigned i = 0; i < 3; ++i)
            endpoint[i] = vk_cc_bc6_endpoint(color[i], format == VK_FORMAT_BC6H_SFLOAT_BLOCK);
        vk_cc_bits(block, &bit, 3, 5); // Mode 11: two explicit 10-bit RGB endpoints.
        for (unsigned e = 0; e < 2; ++e)
            for (unsigned i = 0; i < 3; ++i) vk_cc_bits(block, &bit, endpoint[i], 10);
        break; // All 16 four-bit indices (three bits for the anchor) are zero.
    }
    case VK_FORMAT_BC7_UNORM_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK: {
        unsigned bit = 0;
        vk_cc_bits(block, &bit, 1u << 5, 6); // Mode 5: one subset, separate alpha.
        vk_cc_bits(block, &bit, 0, 2);       // No channel rotation.
        for (unsigned i = 0; i < 4; ++i) {
            unsigned value = vk_cc_unorm(rgba[i], i == 3 ? 255 : 127);
            for (unsigned e = 0; e < 2; ++e) vk_cc_bits(block, &bit, value, i == 3 ? 8 : 7);
        }
        break; // Both 31-bit index planes are zero, including their anchor bits.
    }
    default: break; // The accepted BC range is contiguous and covered above.
    }
    return size;
}
