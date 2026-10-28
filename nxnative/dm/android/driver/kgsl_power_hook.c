// Loaded only in the opted-in custom driver's namespace; constraints follow its own GPU work.
#define _GNU_SOURCE
#include <android/log.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

struct kgsl_create { uint32_t flags, id; };
struct kgsl_constraint { uint32_t type, context_id; void *data; size_t size; };
struct kgsl_property { uint32_t type; void *value; size_t size; };
#define KGSL_CREATE _IOWR(0x09, 0x13, struct kgsl_create)
#define KGSL_SETPROPERTY _IOW(0x09, 0x32, struct kgsl_property)
#define KGSL_CONTEXT_PWR_CONSTRAINT 0x800u
_Static_assert(sizeof(struct kgsl_constraint) == 24 && sizeof(struct kgsl_property) == 24, "KGSL ARM64 ABI");
#ifdef __ANDROID__
_Static_assert(KGSL_CREATE == 0xc0080913ul && KGSL_SETPROPERTY == 0x40180932ul, "KGSL ioctl ABI");
#endif

#ifdef __ANDROID__
__attribute__((visibility("default"))) int ioctl(int fd, int request, ...) {
#else
__attribute__((visibility("default"))) int ioctl(int fd, unsigned long request, ...) {
#endif
    va_list ap; va_start(ap, request); void *arg = va_arg(ap, void *); va_end(ap);
    unsigned long op = (uint32_t)request;
    if (op != KGSL_CREATE || !arg) return (int)syscall(SYS_ioctl, fd, op, arg);
    struct kgsl_create *create = arg;
    uint32_t original = create->flags;
    create->flags |= KGSL_CONTEXT_PWR_CONSTRAINT;
    int result = (int)syscall(SYS_ioctl, fd, op, arg);
    if (result < 0) {
        create->flags = original;
        if (errno != EINVAL) return result;
        return (int)syscall(SYS_ioctl, fd, op, arg);
    }
    int saved_errno = errno;
    uint32_t maximum = 1; // KGSL_CONSTRAINT_PWR_MAX: highest existing level, still subject to thermal/max limits.
    struct kgsl_constraint constraint = {1, create->id, &maximum, sizeof maximum};
    struct kgsl_property property = {0x12, &constraint, sizeof constraint};
    int rc = (int)syscall(SYS_ioctl, fd, KGSL_SETPROPERTY, &property);
    __android_log_print(ANDROID_LOG_INFO, "nx2a-power", "GPU context %u power request %s (errno %d)", create->id, rc ? "refused" : "accepted", rc ? errno : 0);
    errno = saved_errno;
    return result;
}
