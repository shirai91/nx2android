// Android port: the one sysctl use, the process start time (KERN_PROC_PID), from /proc/self/stat
#pragma once
#include <stddef.h>
#include <sys/time.h>
#define CTL_KERN 1
#define KERN_PROC 14
#define KERN_PROC_PID 1
struct extern_proc { struct timeval p_starttime; };
struct kinfo_proc { struct extern_proc kp_proc; };
int sysctl(int *name, unsigned namelen, void *old, size_t *oldlen, void *newp, size_t newlen);
int sysctlbyname(const char *name, void *old, size_t *oldlen, void *newp, size_t newlen);
