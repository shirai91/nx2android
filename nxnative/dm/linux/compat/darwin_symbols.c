// Android port: Darwin functions that must exist as linkable symbols (called from assembly, or declared extern by the host), not only as the
// inline versions of the compat headers. This file deliberately includes none of those headers.
#include <stddef.h>
#include <stdint.h>
#include <time.h>

uint64_t mach_absolute_time(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
// This thread's cumulative {instructions, cycles} (a libSystem private call on Darwin). Not available to apps here: the host's counters stay 0.
int thread_selfcounts(int type, void *buf, size_t nbytes) { (void)type; (void)buf; (void)nbytes; return -1; }
