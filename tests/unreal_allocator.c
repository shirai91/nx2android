// Exercise the production Unreal allocator with an eight-byte tiny size class,
// as in Android jemalloc, independently of this machine's native allocator.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#define LOG(...) ((void)0)
#define die(...) abort()
#define malloc_zone_t test_malloc_zone_t
typedef int malloc_zone_t;
static struct { void *raw, *ptr; size_t size; } blocks[32];
static int fail_next;
static int block(void *p) {
    for (int i = 0; i < 32; ++i) if (blocks[i].ptr == p) return i;
    abort();
}
static void *new_block(size_t n, size_t alignment, bool tiny) {
    if (fail_next) { fail_next = 0; return NULL; }
    void *raw = malloc(n + alignment + 16); assert(raw);
    uintptr_t at = ((uintptr_t)raw + alignment - 1) & ~(uintptr_t)(alignment - 1);
    if (tiny && !(at & 15)) at += 8;
    for (int i = 0; i < 32; ++i) if (!blocks[i].ptr) {
        blocks[i].raw = raw; blocks[i].ptr = (void *)at; blocks[i].size = n;
        return (void *)at;
    }
    abort();
}
static size_t malloc_size(void *p) { return blocks[block(p)].size; }
static size_t malloc_good_size(size_t n) { return (n + 15) & ~(size_t)15; }
static void *malloc_zone_malloc(malloc_zone_t *z, size_t n) {
    (void)z; return new_block(n, n < 16 ? 8 : 16, n < 16);
}
static void *malloc_zone_memalign(malloc_zone_t *z, size_t a, size_t n) {
    (void)z; return new_block(n, a, false);
}
static void malloc_zone_free(malloc_zone_t *z, void *p) {
    (void)z; int i = block(p); free(blocks[i].raw); memset(&blocks[i], 0, sizeof blocks[i]);
}
static void *malloc_zone_realloc(malloc_zone_t *z, void *p, size_t n) {
    void *q = malloc_zone_malloc(z, n);
    if (q) { size_t old = malloc_size(p); memcpy(q, p, old < n ? old : n); malloc_zone_free(z, p); }
    return q;
}

PRODUCTION_ALLOCATOR

static void aligned(void *p, size_t alignment) {
    assert(p && !((uintptr_t)p & (alignment - 1)));
}
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "alloc")) {
        size_t sizes[] = {8, 0, 1, 15, 16, 17, 31, 32};
        unsigned alignments[] = {0, 8, 16, 32, 64};
        for (size_t i = 0; i < sizeof sizes / sizeof *sizes; ++i)
            for (size_t j = 0; j < sizeof alignments / sizeof *alignments; ++j) {
                size_t n = sizes[i], a = alignments[j] > 16 ? alignments[j] : 16, usable = 0;
                void *p = m_Malloc(NULL, n, alignments[j]); aligned(p, a);
                assert(m_GetAllocationSize(NULL, p, &usable) && usable >= (n ? n : 1));
                memset(p, 0x5a, n ? n : 1); m_Free(NULL, p);
                p = m_TryMalloc(NULL, n, alignments[j]); aligned(p, a); m_Free(NULL, p);
            }
    } else if (!strcmp(argv[1], "realloc")) {
        unsigned alignments[] = {0, 16, 32, 64};
        for (size_t j = 0; j < sizeof alignments / sizeof *alignments; ++j) {
            unsigned a = alignments[j];
            // Both successful APIs must preserve alignment when shrinking into
            // the native allocator's tiny bin, and preserve the live bytes.
            for (int attempt = 0; attempt < 2; ++attempt) {
                unsigned char *p = m_Malloc(NULL, 32, a); memset(p, 0x6b, 32);
                p = attempt ? m_TryRealloc(NULL, p, 8, a) : m_Realloc(NULL, p, 8, a);
                aligned(p, a > 16 ? a : 16);
                for (int i = 0; i < 8; ++i) assert(p[i] == 0x6b);
                p = m_Realloc(NULL, p, 128, a); aligned(p, a > 16 ? a : 16);
                for (int i = 0; i < 8; ++i) assert(p[i] == 0x6b);
                assert(m_Realloc(NULL, p, 0, a) == NULL);
            }
        }
        void *p = m_Realloc(NULL, NULL, 8, 0); aligned(p, 16); m_Free(NULL, p);
    } else {
        assert(!strcmp(argv[1], "failure"));
        unsigned char *p = m_Malloc(NULL, 32, 0); memset(p, 0x7c, 32);
        fail_next = 1; assert(m_TryRealloc(NULL, p, 128, 0) == NULL);
        for (int i = 0; i < 32; ++i) assert(p[i] == 0x7c);
        assert(malloc_size(p) == 32); m_Free(NULL, p);
        fail_next = 1; assert(m_TryMalloc(NULL, 8, 0) == NULL);
        assert(!m_GetAllocationSize(NULL, NULL, NULL));
        m_Free(NULL, NULL);
    }
    assert(atomic_load(&g_used) == 0);
    for (int i = 0; i < 32; ++i) assert(!blocks[i].ptr);
    puts("Unreal allocator contract passed");
}
