// Android port: malloc zones are the one bionic heap (an own zone only kept Unreal's allocations apart for accounting); malloc_size = malloc_usable_size.
#pragma once
#include <malloc.h>
#include <stdlib.h>
typedef struct dm_malloc_zone { int unused; } malloc_zone_t;
static inline malloc_zone_t *malloc_create_zone(size_t start, unsigned flags) { (void)start; (void)flags; static malloc_zone_t z; return &z; }
static inline malloc_zone_t *malloc_default_zone(void) { static malloc_zone_t z; return &z; }
static inline void malloc_set_zone_name(malloc_zone_t *z, const char *n) { (void)z; (void)n; }
static inline void *malloc_zone_malloc(malloc_zone_t *z, size_t n) { (void)z; return malloc(n); }
static inline void *malloc_zone_calloc(malloc_zone_t *z, size_t c, size_t n) { (void)z; return calloc(c, n); }
static inline void *malloc_zone_memalign(malloc_zone_t *z, size_t a, size_t n) { (void)z; return memalign(a, n); }
static inline void *malloc_zone_realloc(malloc_zone_t *z, void *p, size_t n) { (void)z; return realloc(p, n); }
static inline void malloc_zone_free(malloc_zone_t *z, void *p) { (void)z; free(p); }
static inline size_t malloc_size(const void *p) { return p ? malloc_usable_size((void *)p) : 0; }
