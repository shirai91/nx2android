// Android port: force-included (-include) into every host source. Darwin library calls with a direct bionic counterpart.
#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>

#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
// Darwin's pthread_setname_np names only the calling thread (one argument); Linux takes the thread too
int dm_setname_self(const char *name);
#define DM_SETNAME_PICK(_1, _2, NAME, ...) NAME
#define pthread_setname_np(...) DM_SETNAME_PICK(__VA_ARGS__, pthread_setname_np, dm_setname_self, x)(__VA_ARGS__)
// the calling thread's stack: Darwin returns its top (highest address) and size
void *pthread_get_stackaddr_np(pthread_t t);
size_t pthread_get_stacksize_np(pthread_t t);
int pthread_threadid_np(pthread_t t, uint64_t *id);
static inline int getdtablesize(void) { return (int)sysconf(_SC_OPEN_MAX); }
#include <time.h>
#ifndef DM_CLOCK_NSEC_NP
#define DM_CLOCK_NSEC_NP
static inline uint64_t clock_gettime_nsec_np(clockid_t c) { struct timespec t; if (clock_gettime(c, &t)) return 0; return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
#endif
static inline size_t malloc_good_size(size_t n) { return (n + 15) & ~(size_t)15; }
#include <pthread/qos.h>   // Darwin's <pthread.h> brings the QoS classes with it
// a wait with a relative timeout (a default condition variable measures CLOCK_REALTIME)
static inline int pthread_cond_timedwait_relative_np(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *rel) {
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += rel->tv_sec; t.tv_nsec += rel->tv_nsec; if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(c, m, &t);
}
#define MACH_PORT_NULL 0
// BSD's stable merge sort (hle_hid.c's DM_KEYS steps): bionic has no mergesort (darwin_compat.c)
int mergesort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
// struct stat's modification time: st_mtimespec on Darwin, st_mtim on Linux
#define st_mtimespec st_mtim
