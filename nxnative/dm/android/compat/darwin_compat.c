// Android port: the Darwin / Mach calls the host makes, on Linux and bionic (headers in dm/android/compat/include).
#include <mach/mach.h>
#include <mach-o/getsect.h>
#include <pthread/qos.h>
#include <CoreFoundation/CoreFoundation.h>
#include <sys/clonefile.h>
#include <sys/sysctl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

// ---------------- ports ----------------
mach_port_t mach_task_self(void) { return getpid(); }
mach_port_t mach_thread_self(void) { return gettid(); }
kern_return_t mach_port_deallocate(task_t task, mach_port_t name) { (void)task; (void)name; return KERN_SUCCESS; }
mach_port_t pthread_mach_thread_np(pthread_t t) { return pthread_gettid_np(t); }
int pthread_threadid_np(pthread_t t, uint64_t *id) { *id = (uint64_t)(t ? pthread_gettid_np(t) : gettid()); return 0; }
int dm_setname_self(const char *name) { char n[16]; snprintf(n, sizeof n, "%s", name); return pthread_setname_np(pthread_self(), n); }
static int stack_of(pthread_t t, uint8_t **lo, size_t *size) {
    pthread_attr_t a; if (pthread_getattr_np(t, &a)) return -1;
    void *p; int r = pthread_attr_getstack(&a, &p, size); pthread_attr_destroy(&a); *lo = p; return r;
}
void *pthread_get_stackaddr_np(pthread_t t) { uint8_t *lo; size_t n; return stack_of(t, &lo, &n) ? NULL : lo + n; }
size_t pthread_get_stacksize_np(pthread_t t) { uint8_t *lo; size_t n; return stack_of(t, &lo, &n) ? 0 : n; }

// ---------------- memory ----------------
// A read that fails instead of faulting: the kernel copies through the page tables and reports EFAULT for an unmapped address
kern_return_t vm_read_overwrite(task_t task, vm_address_t addr, vm_size_t size, vm_address_t data, vm_size_t *outsize) {
    (void)task;
    struct iovec l = {(void *)data, size}, r = {(void *)addr, size};
    ssize_t n = process_vm_readv(getpid(), &l, 1, &r, 1, 0);
    if (n < 0) { if (outsize) *outsize = 0; return KERN_INVALID_ADDRESS; }
    if (outsize) *outsize = (vm_size_t)n;
    return (vm_size_t)n == size ? KERN_SUCCESS : KERN_INVALID_ADDRESS;
}
kern_return_t vm_allocate(task_t task, vm_address_t *addr, vm_size_t size, int flags) {
    (void)task;
    void *hint = (flags & VM_FLAGS_ANYWHERE) ? NULL : (void *)*addr;
    void *p = mmap(hint, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | (hint ? MAP_FIXED : 0), -1, 0);
    if (p == MAP_FAILED) return KERN_NO_SPACE;
    *addr = (vm_address_t)p; return KERN_SUCCESS;
}
kern_return_t vm_deallocate(task_t task, vm_address_t addr, vm_size_t size) { (void)task; return munmap((void *)addr, size) ? KERN_INVALID_ADDRESS : KERN_SUCCESS; }
static int prot_of(const char *perm) { return (perm[0] == 'r' ? VM_PROT_READ : 0) | (perm[1] == 'w' ? VM_PROT_WRITE : 0) | (perm[2] == 'x' ? VM_PROT_EXECUTE : 0); }
// the region containing addr, or the next one above it (as Mach answers), from /proc/self/maps
static int region_at(uint64_t addr, uint64_t *lo, uint64_t *hi, int *prot, int *shared) {
    FILE *f = fopen("/proc/self/maps", "re"); if (!f) return -1;
    char line[512]; int found = -1;
    while (fgets(line, sizeof line, f)) {
        unsigned long long a, b; char perm[8];
        if (sscanf(line, "%llx-%llx %7s", &a, &b, perm) != 3 || b <= addr) continue;
        *lo = a; *hi = b; *prot = prot_of(perm); *shared = perm[3] == 's'; found = 0; break;
    }
    fclose(f); return found;
}
kern_return_t vm_region_64(task_t task, vm_address_t *addr, vm_size_t *size, vm_region_flavor_t flavor, vm_region_info_t info, mach_msg_type_number_t *count, mach_port_t *obj) {
    (void)task; (void)count; uint64_t lo, hi; int prot, shared;
    if (flavor != VM_REGION_BASIC_INFO_64 || region_at(*addr, &lo, &hi, &prot, &shared)) return KERN_INVALID_ADDRESS;
    vm_region_basic_info_data_64_t *bi = (vm_region_basic_info_data_64_t *)info; memset(bi, 0, sizeof *bi);
    bi->protection = prot; bi->max_protection = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE; bi->shared = shared;
    *addr = (vm_address_t)lo; *size = (vm_size_t)(hi - lo); if (obj) *obj = 0;
    return KERN_SUCCESS;
}
kern_return_t mach_vm_region(task_t task, mach_vm_address_t *addr, mach_vm_size_t *size, vm_region_flavor_t flavor, vm_region_info_t info, mach_msg_type_number_t *count, mach_port_t *obj) {
    vm_address_t a = (vm_address_t)*addr; vm_size_t s = 0;
    kern_return_t r = vm_region_64(task, &a, &s, flavor, info, count, obj); *addr = a; *size = s; return r;
}
// Mapping the same pages at a second address needs shared memory; private anonymous memory (what the host maps) cannot be aliased on Linux.
// copy=TRUE (a snapshot at the target) is an eager copy into fresh pages
kern_return_t vm_remap(task_t target, vm_address_t *addr, vm_size_t size, vm_address_t mask, int flags, task_t src_task, vm_address_t src, boolean_t copy,
                       vm_prot_t *cur, vm_prot_t *max, vm_inherit_t inherit) {
    (void)target; (void)mask; (void)src_task; (void)inherit;
    if (!copy) return KERN_NOT_SUPPORTED;
    void *hint = (flags & VM_FLAGS_ANYWHERE) ? NULL : (void *)*addr;
    void *p = mmap(hint, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | (hint ? MAP_FIXED : 0), -1, 0);
    if (p == MAP_FAILED) return KERN_NO_SPACE;
    memcpy(p, (void *)src, size);
    *addr = (vm_address_t)p; if (cur) *cur = VM_PROT_READ | VM_PROT_WRITE; if (max) *max = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;
    return KERN_SUCCESS;
}
kern_return_t mach_vm_remap(task_t target, mach_vm_address_t *addr, mach_vm_size_t size, mach_vm_address_t mask, int flags, task_t src_task, mach_vm_address_t src,
                            boolean_t copy, vm_prot_t *cur, vm_prot_t *max, vm_inherit_t inherit) {
    vm_address_t a = (vm_address_t)*addr;
    kern_return_t r = vm_remap(target, &a, (vm_size_t)size, (vm_address_t)mask, flags, src_task, (vm_address_t)src, copy, cur, max, inherit);
    *addr = a; return r;
}

// ---------------- thread and task information ----------------
static clockid_t thread_cpu_clock(pid_t tid) { return (clockid_t)((~(unsigned)tid << 3) | 6); }   // the kernel's per-thread CPU clock (CPUCLOCK_SCHED of that thread)
static char thread_state_char(pid_t tid) {
    char p[64], b[512]; snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
    int fd = open(p, O_RDONLY | O_CLOEXEC); if (fd < 0) return '?';
    ssize_t n = read(fd, b, sizeof b - 1); close(fd); if (n <= 0) return '?'; b[n] = 0;
    char *e = strrchr(b, ')'); return e && e[1] && e[2] ? e[2] : '?';
}
kern_return_t thread_info(thread_act_t t, thread_flavor_t flavor, thread_info_t out, mach_msg_type_number_t *count) {
    (void)count; struct timespec ts;
    if (clock_gettime(thread_cpu_clock(t), &ts)) return KERN_INVALID_ARGUMENT;
    char st = thread_state_char(t);
    if (flavor == THREAD_BASIC_INFO) {
        thread_basic_info_data_t *bi = (thread_basic_info_data_t *)out; memset(bi, 0, sizeof *bi);
        bi->user_time.seconds = (integer_t)ts.tv_sec; bi->user_time.microseconds = (integer_t)(ts.tv_nsec / 1000);   // user + system together: the kernel's clock does not split them
        bi->run_state = st == 'R' ? TH_STATE_RUNNING : TH_STATE_WAITING;
        return KERN_SUCCESS;
    }
    if (flavor == THREAD_EXTENDED_INFO) {
        thread_extended_info_data_t *xi = (thread_extended_info_data_t *)out; memset(xi, 0, sizeof *xi);
        xi->pth_user_time = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; xi->pth_run_state = st == 'R' ? TH_STATE_RUNNING : TH_STATE_WAITING;
        xi->pth_curpri = xi->pth_priority = -getpriority(PRIO_PROCESS, (id_t)t);
        return KERN_SUCCESS;
    }
    return KERN_INVALID_ARGUMENT;
}
kern_return_t task_info(task_t task, task_flavor_t flavor, task_info_t out, mach_msg_type_number_t *count) {
    (void)task; (void)count;
    if (flavor != TASK_VM_INFO) return KERN_FAILURE;   // no energy / power accounting for apps on Linux
    task_vm_info_data_t *vi = (task_vm_info_data_t *)out; memset(vi, 0, sizeof *vi);
    FILE *f = fopen("/proc/self/statm", "re"); unsigned long long vsz = 0, rss = 0;
    if (!f) return KERN_FAILURE;
    int ok = fscanf(f, "%llu %llu", &vsz, &rss) == 2; fclose(f);
    if (!ok) return KERN_FAILURE;
    uint64_t pg = (uint64_t)sysconf(_SC_PAGESIZE);
    vi->virtual_size = vsz * pg; vi->resident_size = vi->phys_footprint = vi->internal = rss * pg; vi->page_size = (integer_t)pg;
    return KERN_SUCCESS;
}
kern_return_t task_threads(task_t task, thread_act_array_t *list, mach_msg_type_number_t *count) {
    (void)task;
    DIR *d = opendir("/proc/self/task"); if (!d) return KERN_FAILURE;
    size_t cap = 4096; mach_port_t *a = mmap(NULL, cap * sizeof *a, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);   // freed by vm_deallocate (munmap)
    if (a == MAP_FAILED) { closedir(d); return KERN_NO_SPACE; }
    unsigned n = 0; struct dirent *e;
    while ((e = readdir(d)) && n < cap) if (e->d_name[0] >= '0' && e->d_name[0] <= '9') a[n++] = atoi(e->d_name);
    closedir(d);
    // the callers give back n entries' worth (vm_deallocate(n * sizeof *list), as on Darwin): the pages past that go now, or they stayed mapped (12 KB of address space a call)
    size_t pg = (size_t)getpagesize(), used = (n * sizeof *a + pg - 1) & ~(pg - 1), all = (cap * sizeof *a + pg - 1) & ~(pg - 1);
    if (used < all) munmap((uint8_t *)a + used, all - used);
    *list = a; *count = n; return KERN_SUCCESS;
}
int thread_policy_set(int thread, thread_policy_flavor_t flavor, thread_policy_t policy, unsigned count) { (void)thread; (void)flavor; (void)policy; (void)count; return KERN_NOT_SUPPORTED; }
int thread_policy_get(int thread, thread_policy_flavor_t flavor, thread_policy_t policy, unsigned *count, int *get_default) {
    (void)thread; (void)flavor; (void)count; if (get_default) *get_default = 1;
    memset(policy, 0, sizeof(thread_time_constraint_policy_data_t)); return KERN_NOT_SUPPORTED;
}

// ---------------- thread suspension (thread dumps) ----------------
// thread_suspend sends SIG_PARK; the target's handler (on its signal stack) records its registers and waits on a futex until thread_resume.
// A thread blocked in a system call is interrupted there; SA_RESTART resumes the call afterwards.
// A slot is free again only once its thread has left the handler (state 3 → 0, set by the handler): thread_resume used to free it at once and wake one waiter, so the
// next suspend of a thread dump took the same slot and set it to 1 → 2 before the released thread ran again; that thread saw 2, went back to its wait and was never woken
// (Kirby and the Forgotten Land, 2026-11-09: a guest thread left parked for good by a DM_CTL "threads" dump).
#define MAX_PARK 64
typedef struct { _Atomic int tid; _Atomic int state; arm_thread_state64_t regs; } park_t;   // state: 1 asked, 2 parked, 3 released, 0 left the handler (slot free once tid is 0)
static park_t g_park[MAX_PARK];
static int sig_park(void) { return SIGRTMIN + 5; }
static long futex(_Atomic int *a, int op, int v, const struct timespec *t) { return syscall(SYS_futex, a, op, v, t, NULL, 0); }
static park_t *park_of(pid_t tid, int state) { for (int i = 0; i < MAX_PARK; i++) if (atomic_load(&g_park[i].tid) == tid && atomic_load(&g_park[i].state) == state) return &g_park[i]; return NULL; }   // by state too: a slot kept by a resume that timed out holds the same tid
static void on_park(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si; int e = errno;
    park_t *p = park_of(gettid(), 1);
    if (p) {
        ucontext_t *uc = uc_; memcpy(&p->regs, &uc->uc_mcontext.regs[0], sizeof p->regs);   // x0..x28, fp, lr, sp, pc, pstate: Darwin's order (see dm_uc.h)
        atomic_store(&p->state, 2); futex(&p->state, FUTEX_WAKE_PRIVATE, INT_MAX, NULL);
        while (atomic_load(&p->state) == 2) futex(&p->state, FUTEX_WAIT_PRIVATE, 2, NULL);
        atomic_store(&p->state, 0); futex(&p->state, FUTEX_WAKE_PRIVATE, INT_MAX, NULL);   // left the handler: thread_resume may free the slot
    }
    errno = e;
}
static void park_install_once(void) {
    struct sigaction sa = {0}; sa.sa_sigaction = on_park; sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART; sigfillset(&sa.sa_mask); sigaction(sig_park(), &sa, NULL);
}
static void park_install(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, park_install_once); }
kern_return_t thread_suspend(thread_act_t t) {
    park_install();
    if (t == gettid()) return KERN_INVALID_ARGUMENT;
    park_t *p = NULL;
    for (int i = 0; i < MAX_PARK && !p; i++) { int z = 0; if (atomic_compare_exchange_strong(&g_park[i].tid, &z, t)) p = &g_park[i]; }
    if (!p) return KERN_NO_SPACE;
    atomic_store(&p->state, 1);
    if (syscall(SYS_tgkill, getpid(), t, sig_park())) { atomic_store(&p->state, 0); atomic_store(&p->tid, 0); return KERN_INVALID_ARGUMENT; }
    struct timespec to = {0, 200000000};   // 200 ms: a thread that never reaches the handler (signal blocked) is not dumped
    while (atomic_load(&p->state) == 1) if (futex(&p->state, FUTEX_WAIT_PRIVATE, 1, &to) && errno == ETIMEDOUT) break;
    int s = 1;
    if (atomic_compare_exchange_strong(&p->state, &s, 0)) { atomic_store(&p->tid, 0); return KERN_FAILURE; }   // not parked: withdraw the request
    return KERN_SUCCESS;
}
kern_return_t thread_get_state(thread_act_t t, thread_flavor_t flavor, thread_state_t out, mach_msg_type_number_t *count) {
    (void)count; park_t *p = park_of(t, 2);
    if (flavor != ARM_THREAD_STATE64 || !p) return KERN_INVALID_ARGUMENT;
    memcpy(out, &p->regs, sizeof p->regs); return KERN_SUCCESS;
}
kern_return_t thread_resume(thread_act_t t) {
    park_t *p = park_of(t, 2); if (!p) return KERN_INVALID_ARGUMENT;
    atomic_store(&p->state, 3); futex(&p->state, FUTEX_WAKE_PRIVATE, INT_MAX, NULL);
    struct timespec to = {0, 200000000};   // until the thread has left the handler; a thread that never runs again keeps its slot (a leak of one of 64, not a hang)
    while (atomic_load(&p->state) == 3) if (futex(&p->state, FUTEX_WAIT_PRIVATE, 3, &to) && errno == ETIMEDOUT) return KERN_SUCCESS;
    atomic_store(&p->tid, 0); return KERN_SUCCESS;
}

// ---------------- QoS classes → nice values ----------------
static __thread qos_class_t t_qos = QOS_CLASS_DEFAULT;
static int qos_nice(qos_class_t q) {
    switch (q) { case QOS_CLASS_USER_INTERACTIVE: return -10; case QOS_CLASS_USER_INITIATED: return -4; case QOS_CLASS_UTILITY: return 4; case QOS_CLASS_BACKGROUND: return 10; default: return 0; }
}
int pthread_set_qos_class_self_np(qos_class_t q, int rel) {
    (void)rel; t_qos = q;
    setpriority(PRIO_PROCESS, (id_t)gettid(), qos_nice(q));   // apps may not go below their limit: then the thread keeps its nice value
    return 0;
}
int pthread_get_qos_class_np(pthread_t t, qos_class_t *q, int *rel) { (void)t; if (q) *q = t_qos; if (rel) *rel = 0; return 0; }
qos_class_t qos_class_self(void) { return t_qos; }

// ---------------- sections ----------------
const struct mach_header_64 _mh_execute_header = {0xfeedfacf};
#define SECT(n) extern const uint8_t __start_##n[] __attribute__((weak, visibility("hidden"))); extern const uint8_t __stop_##n[] __attribute__((weak, visibility("hidden")));
SECT(swhle) SECT(swforce) SECT(swnvn)
uint8_t *getsectiondata(const struct mach_header_64 *mh, const char *seg, const char *sect, unsigned long *size) {
    (void)mh; (void)seg;
    const uint8_t *a = NULL, *b = NULL;
    if (!strcmp(sect, "swhle")) { a = __start_swhle; b = __stop_swhle; }
    else if (!strcmp(sect, "swforce")) { a = __start_swforce; b = __stop_swforce; }
    else if (!strcmp(sect, "swnvn")) { a = __start_swnvn; b = __stop_swnvn; }
    if (!a || !b) { if (size) *size = 0; return NULL; }
    if (size) *size = (unsigned long)(b - a);
    return (uint8_t *)a;
}

// ---------------- CFDictionary (pointer keys, no callbacks) ----------------
struct dm_cfdict { size_t cap, n; const void **k, **v; };
static const void *const TOMB = (const void *)1;
static size_t slot_of(const struct dm_cfdict *d, const void *key) {
    uintptr_t h = (uintptr_t)key; h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33;
    return (size_t)h & (d->cap - 1);
}
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef a, CFIndex cap, const void *kcb, const void *vcb) {
    (void)a; (void)cap; (void)kcb; (void)vcb;
    struct dm_cfdict *d = calloc(1, sizeof *d); d->cap = 64; d->k = calloc(d->cap, sizeof *d->k); d->v = calloc(d->cap, sizeof *d->v); return d;
}
static size_t find(const struct dm_cfdict *d, const void *key) {   // the key's slot, or cap when absent
    for (size_t i = slot_of(d, key), n = 0; n < d->cap; i = (i + 1) & (d->cap - 1), n++) { if (!d->k[i]) return d->cap; if (d->k[i] == key) return i; }
    return d->cap;
}
const void *CFDictionaryGetValue(CFDictionaryRef d, const void *key) { size_t i = find(d, key); return i < d->cap ? d->v[i] : NULL; }
static void put(struct dm_cfdict *d, const void *key, const void *value) {
    size_t i = slot_of(d, key);
    while (d->k[i] && d->k[i] != TOMB) i = (i + 1) & (d->cap - 1);
    d->k[i] = key; d->v[i] = value; d->n++;
}
void CFDictionarySetValue(CFMutableDictionaryRef d, const void *key, const void *value) {
    size_t i = find(d, key); if (i < d->cap) { d->v[i] = value; return; }
    if (2 * (d->n + 1) > d->cap) {   // grow (tombstones are dropped)
        size_t oc = d->cap; const void **ok = d->k, **ov = d->v;
        d->cap *= 2; d->n = 0; d->k = calloc(d->cap, sizeof *d->k); d->v = calloc(d->cap, sizeof *d->v);
        for (size_t j = 0; j < oc; j++) if (ok[j] && ok[j] != TOMB) put(d, ok[j], ov[j]);
        free(ok); free(ov);
    }
    put(d, key, value);
}
void CFDictionaryRemoveValue(CFMutableDictionaryRef d, const void *key) { size_t i = find(d, key); if (i < d->cap) { d->k[i] = TOMB; d->v[i] = NULL; d->n--; } }
CFIndex CFDictionaryGetCount(CFDictionaryRef d) { return (CFIndex)d->n; }

// ---------------- clonefile / renamex_np ----------------
static int copy_file(const char *src, const char *dst, mode_t mode) {
    int in = open(src, O_RDONLY | O_CLOEXEC); if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode & 07777);
    if (out < 0) { int e = errno; close(in); errno = e; return -1; }
    char buf[1 << 16]; ssize_t n; int r = 0;
    while ((n = read(in, buf, sizeof buf)) > 0) for (ssize_t o = 0; o < n;) { ssize_t w = write(out, buf + o, (size_t)(n - o)); if (w < 0) { r = -1; break; } o += w; }
    if (n < 0) r = -1;
    int e = errno; close(in); if (close(out)) r = -1; errno = e; return r;
}
int clonefile(const char *src, const char *dst, uint32_t flags) {
    struct stat st; if ((flags & CLONE_NOFOLLOW ? lstat : stat)(src, &st)) return -1;
    if (!access(dst, F_OK)) { errno = EEXIST; return -1; }
    if (!S_ISDIR(st.st_mode)) return copy_file(src, dst, st.st_mode);
    if (mkdir(dst, st.st_mode & 07777)) return -1;
    DIR *d = opendir(src); if (!d) return -1;
    struct dirent *e; int r = 0;
    while (!r && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[PATH_MAX], b[PATH_MAX]; snprintf(a, sizeof a, "%s/%s", src, e->d_name); snprintf(b, sizeof b, "%s/%s", dst, e->d_name);
        r = clonefile(a, b, flags);
    }
    int er = errno; closedir(d); errno = er; return r;
}
int renamex_np(const char *from, const char *to, unsigned flags) {
    unsigned lf = (flags & RENAME_SWAP ? 2u /* RENAME_EXCHANGE */ : 0) | (flags & RENAME_EXCL ? 1u /* RENAME_NOREPLACE */ : 0);
    return (int)syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, lf);
}

// ---------------- sysctl: the process start time ----------------
int sysctl(int *name, unsigned namelen, void *old, size_t *oldlen, void *newp, size_t newlen) {
    (void)newp; (void)newlen;
    if (namelen != 4 || name[0] != CTL_KERN || name[1] != KERN_PROC || name[2] != KERN_PROC_PID || !old || !oldlen || *oldlen < sizeof(struct kinfo_proc)) { errno = ENOTSUP; return -1; }
    char b[1024]; int fd = open("/proc/self/stat", O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    ssize_t n = read(fd, b, sizeof b - 1); close(fd); if (n <= 0) return -1; b[n] = 0;
    char *p = strrchr(b, ')'); if (!p) return -1;
    unsigned long long start = 0; int field = 2;   // after the command: field 3 is the state; starttime is field 22, in clock ticks since boot
    for (char *t = strtok(p + 1, " "); t; t = strtok(NULL, " ")) if (++field == 22) { start = strtoull(t, NULL, 10); break; }
    struct timespec rt, bt; clock_gettime(CLOCK_REALTIME, &rt); clock_gettime(CLOCK_BOOTTIME, &bt);
    double boot = rt.tv_sec + rt.tv_nsec / 1e9 - (bt.tv_sec + bt.tv_nsec / 1e9), at = boot + (double)start / (double)sysconf(_SC_CLK_TCK);
    struct kinfo_proc *kp = old; memset(kp, 0, sizeof *kp);
    kp->kp_proc.p_starttime.tv_sec = (time_t)at; kp->kp_proc.p_starttime.tv_usec = (suseconds_t)((at - (double)(time_t)at) * 1e6);
    *oldlen = sizeof *kp; return 0;
}
int sysctlbyname(const char *name, void *old, size_t *oldlen, void *newp, size_t newlen) { (void)name; (void)old; (void)oldlen; (void)newp; (void)newlen; errno = ENOENT; return -1; }
