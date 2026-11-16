// Android port: Mach absolute time = CLOCK_MONOTONIC in nanoseconds (timebase 1/1).
#pragma once
#include <stdint.h>
#include <time.h>
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t, *mach_timebase_info_t;
static inline int mach_timebase_info(mach_timebase_info_t tb) { tb->numer = 1; tb->denom = 1; return 0; }
static inline uint64_t mach_absolute_time(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static inline uint64_t mach_continuous_time(void) { struct timespec t; clock_gettime(CLOCK_BOOTTIME, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static inline int mach_wait_until(uint64_t deadline) {
    struct timespec t = {(time_t)(deadline / 1000000000ull), (long)(deadline % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL)) ;
    return 0;
}
// Darwin's nanosecond clock read
#ifndef DM_CLOCK_NSEC_NP
#define DM_CLOCK_NSEC_NP
static inline uint64_t clock_gettime_nsec_np(clockid_t c) { struct timespec t; if (clock_gettime(c, &t)) return 0; return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
#endif
