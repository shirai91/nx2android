// Register access in a signal handler's ucontext, the same on Darwin and Linux (Android port).
// DM_UC_SS(uc): the general registers as Darwin's arm_thread_state64 (x0..x28, fp, lr, sp, pc, cpsr), readable and writable. Linux's sigcontext keeps
//   regs[0..30], sp, pc, pstate in that same order and size, so on Android it is a view of uc_mcontext.regs (cpsr = the low half of pstate).
// DM_UC_V(uc) / DM_UC_FPSR(uc) / DM_UC_FPCR(uc): the SIMD registers (Linux: the fpsimd record in __reserved).
// DM_UC_ESR(uc): the fault's exception syndrome (Linux: the esr record in __reserved, 0 when the kernel gave none).
#pragma once
#include <signal.h>
#include <stdint.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>   // Darwin's own header: <ucontext.h> only declares the getcontext/setcontext family, which needs _XOPEN_SOURCE and a newer macOS SDK refuses it
#else
#include <ucontext.h>
#endif
#if defined(__APPLE__)
#define DM_UC_SS(uc) (&(uc)->uc_mcontext->__ss)
#define DM_UC_V(uc) ((uc)->uc_mcontext->__ns.__v)
#define DM_UC_FPSR(uc) ((uc)->uc_mcontext->__ns.__fpsr)
#define DM_UC_FPCR(uc) ((uc)->uc_mcontext->__ns.__fpcr)
#define DM_UC_ESR(uc) ((uc)->uc_mcontext->__es.__esr)
#else
#include <mach/mach.h>   // struct __darwin_arm_thread_state64 (dm/android/compat)
#include <asm/sigcontext.h>
_Static_assert(__builtin_offsetof(struct sigcontext, pc) - __builtin_offsetof(struct sigcontext, regs) == __builtin_offsetof(struct __darwin_arm_thread_state64, __pc), "sigcontext layout");
#define DM_UC_SS(uc) ((struct __darwin_arm_thread_state64 *)(void *)&(uc)->uc_mcontext.regs[0])
static inline void *dm_uc_record(const ucontext_t *uc, uint32_t magic) {   // a record of the __reserved area: {magic, size} headers, ended by magic 0
    const uint8_t *p = (const uint8_t *)uc->uc_mcontext.__reserved, *end = p + sizeof uc->uc_mcontext.__reserved;
    while (p + 8 <= end) {
        const struct _aarch64_ctx *h = (const struct _aarch64_ctx *)p;
        if (!h->magic || !h->size) return 0;
        if (h->magic == magic) return (void *)p;
        p += h->size;
    }
    return 0;
}
static inline struct fpsimd_context *dm_uc_fpsimd(const ucontext_t *uc) {
    static struct fpsimd_context none;   // a context without the record (never on arm64 Linux): reads give zeros
    struct fpsimd_context *f = dm_uc_record(uc, FPSIMD_MAGIC); return f ? f : &none;
}
#define DM_UC_V(uc) (dm_uc_fpsimd(uc)->vregs)
#define DM_UC_FPSR(uc) (dm_uc_fpsimd(uc)->fpsr)
#define DM_UC_FPCR(uc) (dm_uc_fpsimd(uc)->fpcr)
static inline uint64_t dm_uc_esr(const ucontext_t *uc) { struct esr_context *e = dm_uc_record(uc, ESR_MAGIC); return e ? e->esr : 0; }
#define DM_UC_ESR(uc) dm_uc_esr(uc)
#endif
