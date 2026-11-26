#pragma once
#include <stddef.h>
#include <stdint.h>

#define LSFG_SHADER_COUNT 25
#define LSFG_DLL_MAX_SIZE (32u * 1024 * 1024)

typedef struct {
    const uint32_t *words;
    size_t size;
} lsfg_shader;

// The returned views point into the caller's DLL buffer, which must stay alive.
// Each complete set has resources 255, 256, and 280 through 302 plus its offset.
int lsfg_dll_shaders(const void *data, size_t size, unsigned offset,
                     lsfg_shader shaders[LSFG_SHADER_COUNT]);
