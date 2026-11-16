// Android port: instruction cache maintenance after writing code
#pragma once
#include <stddef.h>
static inline void sys_icache_invalidate(void *start, size_t len) { __builtin___clear_cache((char *)start, (char *)start + len); }
static inline void sys_dcache_flush(void *start, size_t len) { (void)start; (void)len; }
