    .arch_extension lse
// Common entry for unimplemented NVN functions: when the thunk comes in, x16 = this function's slot address (nvn_slot[i]).
// Calls nvn_unimpl_c(slot, x0, x1, x2) to report a failure and log the name, returns 0.
    .text
    .globl nvn_unimpl_entry
    .p2align 2
nvn_unimpl_entry:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x3, x2
    mov x2, x1
    mov x1, x0
    mov x0, x16
    bl nvn_unimpl_c
    ldp x29, x30, [sp], #16
    ret

// Timing wrapper entry (the slot table points here when DM_NVN_PROF=1): when the thunk comes in, x16 = this function's slot address.
// Save x0-x7 / q0-q7 (NVN functions pass no arguments on the stack, verified by the generator), take mach_absolute_time -> call the real implementation (nvn_real[i]) -> take it again,
// atomically add the difference to nvn_prof_ticks[i] (mach timebase units); return values x0/x1, q0-q3 are passed back unchanged.
// cntvct_el0 is not used: measured on this machine it is not in a fixed ratio to the system timebase (cntfrq reports 1 GHz, increment ratio over the same interval 98.9), so it cannot be converted to time.
    .globl nvn_prof_entry
    .p2align 2
nvn_prof_entry:
    sub sp, sp, #240
    stp x29, x30, [sp]
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    adrp x9, nvn_slot
    add x9, x9, :lo12:nvn_slot
    sub x16, x16, x9
    lsr x16, x16, #4                 // Slot index i
    str x16, [sp, #80]
    bl mach_absolute_time
    str x0, [sp, #88]
    ldr x16, [sp, #80]
    adrp x9, nvn_real
    add x9, x9, :lo12:nvn_real
    ldr x17, [x9, x16, lsl #3]
    adrp x9, nvn_slot          // x16 back to the slot address: when the real implementation is the big-stack entry (nvn_big_entry) it finds the implementation through it
    add x9, x9, :lo12:nvn_slot
    add x16, x9, x16, lsl #4
    ldp x0, x1, [sp, #16]
    ldp x2, x3, [sp, #32]
    ldp x4, x5, [sp, #48]
    ldp x6, x7, [sp, #64]
    ldp q0, q1, [sp, #96]
    ldp q2, q3, [sp, #128]
    ldp q4, q5, [sp, #160]
    ldp q6, q7, [sp, #192]
    blr x17
    stp x0, x1, [sp, #16]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    bl mach_absolute_time
    mov x9, x0
    ldr x10, [sp, #88]
    sub x9, x9, x10
    ldr x16, [sp, #80]
    adrp x10, nvn_prof_ticks
    add x10, x10, :lo12:nvn_prof_ticks
    add x10, x10, x16, lsl #3
    stadd x9, [x10]
    ldp x0, x1, [sp, #16]
    ldp q0, q1, [sp, #96]
    ldp q2, q3, [sp, #128]
    ldp x29, x30, [sp]
    add sp, sp, #240
    ret

// Big-stack entry: when the thunk comes in, x16 = this function's slot address. Load nvn_big_real[i] into x16 and hand over to dm_bigstack_enter (dm/host/hle_bigstack.c):
// on a guest thread stack it switches to this thread's big host stack before calling the implementation. The host's NVN implementations do Metal work when submitting, presenting and creating textures, and the Metal driver compiles internal programs (the fast-clear blit program, ...) on a deep stack,
// which The Legend of Zelda: Breath of the Wild's render thread stack cannot hold (SIGBUS in the AGX compiler, at an address in the guest stack region). NVN functions pass no arguments on the stack (verified by the generator); x0-x8 / d0-d7 are enough
    .globl nvn_big_entry
    .p2align 2
nvn_big_entry:
    adrp x17, nvn_slot
    add x17, x17, :lo12:nvn_slot
    sub x16, x16, x17
    lsr x16, x16, #4                 // Slot index i
    adrp x17, nvn_big_real
    add x17, x17, :lo12:nvn_big_real
    ldr x16, [x17, x16, lsl #3]
    b dm_bigstack_enter
