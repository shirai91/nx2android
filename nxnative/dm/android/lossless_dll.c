// Read the SPIR-V data resources of a Windows PE file without loading its code.
#include "lossless_dll.h"
#include <string.h>

typedef struct {
    const uint8_t *bytes;
    size_t size;
} span;

static span slice(span s, size_t offset, size_t size) {
    if (!s.bytes || offset > s.size || size > s.size - offset) return (span){0};
    return (span){s.bytes + offset, size};
}

static uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t)le16(p + 2) << 16; }

static span pe_data(span file, span sections, uint32_t rva, uint32_t size) {
    for (size_t i = 0; i < sections.size; i += 40) {
        const uint8_t *s = sections.bytes + i;
        uint32_t address = le32(s + 12), raw_size = le32(s + 16), raw_offset = le32(s + 20);
        if (rva < address) continue;
        uint32_t delta = rva - address;
        if (delta >= raw_size || size > raw_size - delta) continue;
        return slice(slice(file, raw_offset, raw_size), delta, size);
    }
    return (span){0};
}

#define RESOURCE_DIRECTORY 0x80000000u

static span resource_entries(span resources, uint32_t offset) {
    span header = slice(resources, offset, 16);
    if (!header.bytes) return (span){0};
    size_t count = (size_t)le16(header.bytes + 12) + le16(header.bytes + 14);
    return slice(resources, (size_t)offset + 16, count * 8);
}

static int resource_dir(span resources, uint32_t offset, uint32_t id, uint32_t *child) {
    span entries = resource_entries(resources, offset);
    for (size_t i = 0; i < entries.size; i += 8) {
        const uint8_t *p = entries.bytes + i;
        uint32_t value = le32(p + 4);
        if (le32(p) == id && (value & RESOURCE_DIRECTORY)) {
            *child = value & ~RESOURCE_DIRECTORY;
            return 1;
        }
    }
    return 0;
}

static span resource_shader(span file, span sections, span resources, uint32_t type, uint32_t id) {
    uint32_t languages;
    if (!resource_dir(resources, type, id, &languages)) return (span){0};
    span entries = resource_entries(resources, languages);
    for (size_t i = 0; i < entries.size; i += 8) {
        uint32_t offset = le32(entries.bytes + i + 4);
        span leaf = slice(resources, offset, 16);
        if (!leaf.bytes || (offset & RESOURCE_DIRECTORY)) continue;
        span code = pe_data(file, sections, le32(leaf.bytes), le32(leaf.bytes + 4));
        if (code.size >= 20 && code.size % 4 == 0 && (uintptr_t)code.bytes % 4 == 0 &&
            le32(code.bytes) == 0x07230203u) return code;
    }
    return (span){0};
}

int lsfg_dll_shaders(const void *data, size_t size, unsigned offset,
                     lsfg_shader shaders[LSFG_SHADER_COUNT]) {
    memset(shaders, 0, LSFG_SHADER_COUNT * sizeof *shaders);
    span file = {data, size};
    if (!data || size < 64 || size > LSFG_DLL_MAX_SIZE || le16(file.bytes) != 0x5a4d) return 0;
    size_t pe = le32(file.bytes + 60);
    span header = slice(file, pe, 24);
    if (!header.bytes || le32(header.bytes) != 0x4550) return 0;
    size_t optional_size = le16(header.bytes + 20);
    span optional = slice(file, pe + 24, optional_size);
    if (optional.size < 2) return 0;
    uint16_t magic = le16(optional.bytes);
    size_t directories = magic == 0x10b ? 96 : magic == 0x20b ? 112 : 0;
    if (!directories || optional.size < directories + 24 || le32(optional.bytes + directories - 4) < 3) return 0;
    span sections = slice(file, pe + 24 + optional_size, (size_t)le16(header.bytes + 6) * 40);
    const uint8_t *resource_directory = optional.bytes + directories + 16;
    span resources = pe_data(file, sections, le32(resource_directory), le32(resource_directory + 4));
    uint32_t raw_data;
    if (!resource_dir(resources, 0, 10, &raw_data)) return 0;   // RT_RCDATA

    lsfg_shader found[LSFG_SHADER_COUNT];
    for (unsigned i = 0; i < LSFG_SHADER_COUNT; ++i) {
        uint32_t id = i < 2 ? 255 + i : 278 + i;
        span code = resource_shader(file, sections, resources, raw_data, id + offset);
        if (!code.bytes) return 0;
        found[i] = (lsfg_shader){(const uint32_t *)code.bytes, code.size};
    }
    memcpy(shaders, found, sizeof found);
    return 1;
}
