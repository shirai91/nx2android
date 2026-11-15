# Xenoblade Chronicles 2 (0100E95004038000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's
# NSO build ID; FRAME_STEP as described there ("Registered patches", "Frame step in whole vsyncs")
from nso_asm import *

# Xenoblade Chronicles 2 2.1.0 (v983040) main: 60 fps. The engine steps its game time by a frame delta it keeps as a float in three fields ([[main+0xb8f0d0]+0x78] and +0x7c,
# [[main+0xc222c8]+0x80]; 1/30 s as shipped), presents every [main+0xb74bf0] vsyncs (two uint32, 2 as shipped), and takes 1/30 s as two more constants: the wind's speed
# (main+0x747dd8 loads it from main+0x9b4070) and the frame time its dynamic resolution compares the GPU time with (the virtual call at main+0x6aa14c). The sites and fields are those
# of the community's FPSLocker config of this build (masagrator's FPSLocker-Warehouse, 0100E95004038000/F77F1559371C0EC6); the stub below is our own, written for the host:
#   - it runs once a frame, from main+0x700164 (the instruction after the frame's update call; x0-x2, x8-x17 and v0-v7 are dead there, the call clobbered them);
#   - the frame's real time comes from the virtual counter (cntvct_el0 / cntfrq_el0, readable at EL0 on Android and Apple), not from the Switch's tick and its SDK conversion;
#   - the target is the state page's (+0, the loader's fps_target / DM_FPS_TARGET; the host paces the presents by it, hle_nvn.c fstep_tick). 60: the delta is the real time,
#     kept within [1/60, 1/30] s, and the interval 1. 30, or while the game's cutscene flag (the byte at main+0xbd7f40) is 1: delta 1/30 s, interval 2, and the flag is mirrored
#     into the state page's movie flag (+4), which holds the host at 30 fps too (cutscenes are animated for 30). 0: the game's own values are left alone, and after a run at
#     another target they are put back once (interval 2, delta 1/30 s), so the target can change while the game runs;
#   - the dynamic resolution: the game draws its scene at 896x504 instead of 1280x720 (the UI stays 1280x720) while the GPU load main+0x6aa0ec returns is high: the
#     frame's GPU time over the frame time (the delta times the interval's vsyncs) in percent, plus a margin of 3.0 (fmov s1,#3.0 at main+0x6aa18c; Xenoblade Chronicles:
#     Definitive Edition has the same function, its patches.py). At 60 fps on the Odin 3 the load is about 93 and the scene is 896x504 all the time, so a larger margin
#     alone changed nothing there. The margin follows the state page's render scale (+0x10, the host's DM_RENDER_SCALE, 0.5 to 1.0): 1.0, the host's default, keeps the
#     game's own margin; below it the margin is 3 + (0.75 - scale) x 800, so 0.5 (+203) holds the scene at 896x504 and 0.9 (-117) at 1280x720 (the Android pause menu's
#     Resolution row: Low, Auto, High);
#   - the task wait: the frame function waits for the engine's tasks in the loop at main+0x6dea04 (the same loop as Xenoblade Chronicles: Definitive Edition's, its
#     patches.py: while the task still runs it locks the task list, calls every subsystem's pump, vtable +0x48, for those with pending work at +0x648 / +0x650, and goes
#     round again). Its caller picks a pause by a mode in w28: 1 sleeps 1 ms a round, 2 sleeps 30 ms, any other none at all, and that one is the frame's own wait. This
#     build has no yield in it either, so the host's back-off has nothing to count: on the Odin 3 that thread took 96% of a core in a 30 fps cutscene, 40% of it in the
#     clock gate and clock_gettime (simpleperf, task-clock). The branch over the 30 ms sleep (main+0x6debe8, b.ne) goes to a stub that sleeps 131,072 ns in the other
#     modes (nn::os::SleepThread, PLT main+0x8b25f0), as the Definitive Edition's patch does: the wait polls the same task state and pumps the same subsystems, less often;
#   - state page: +0x100 u64 the counter at the last frame, +0x108 float the delta the wind reads, +0x10c u32 1 while the stub's values are in the game's fields.
# Tested 2026-11-14 on the AYN Odin 3 (Snapdragon 8 Elite, Adreno 830)
def frame_step(ctx):
    here, emit, site, put, ST = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    site(0x700164, '08348452')            # mov w8,#0x21a0
    site(0x747dd8, '681300b0067140bd')    # adrp x8,main+0x9b4000; ldr s6,[x8,#0x70]
    site(0x6aa14c, '086940f900013fd6')    # ldr x8,[x8,#0xd0]; blr x8
    site(0x6aa18c, '0110211e')            # fmov s1,#3.0
    lit = here(); emit(0x3c888889)        # 1/60 s: the dynamic resolution's frame time (the interval's vsyncs times the delta at either target)
    start = here()
    adrp = {1: (16, ST), 2: (2, 0xbd7000), 3: (2, 0xb74000), 4: (0, 0xb8f000), 5: (0, 0xc22000)}   # the brk #n placeholders: adrp of (register, page)
    for w in (0xd4200020,   # adrp x16,state
              0xd53be040,   # mrs x0,cntvct_el0
              0xf9408201,   # ldr x1,[x16,#0x100]      the counter at the last frame
              0xf9008200,   # str x0,[x16,#0x100]
              0xb9400211,   # ldr w17,[x16]            target fps (float bits; 0 = off)
              0x1e270222,   # fmov s2,w17
              0x52911129,   # mov w9,#0x8889
              0x72a7a109,   # movk w9,#0x3d08,lsl #16
              0x1e270123,   # fmov s3,w9               1/30 s
              0x34000551,   # cbz w17,off
              0x52800029,   # mov w9,#1
              0xb9010e09,   # str w9,[x16,#0x10c]      the stub's values are in the game's fields
              0xd4200040,   # adrp x2,main+0xbd7000
              0x397d0042,   # ldrb w2,[x2,#0xf40]      the cutscene flag
              0x7100045f,   # cmp w2,#1
              0x1a9f17e2,   # cset w2,eq
              0xb9000602,   # str w2,[x16,#4]          the host's movie flag: 30 fps while it is set
              0x1e27f000,   # fmov s0,#31.0
              0x1e202040,   # fcmp s2,s0
              0x1a9fa7f1,   # cset w17,lt              a 30 fps target
              0x2a110042,   # orr w2,w2,w17
              0x11000449,   # add w9,w2,#1             the interval: 2 in cutscenes and at 30, else 1
              0xd4200060,   # adrp x2,main+0xb74000
              0xb90bf049,   # str w9,[x2,#0xbf0]
              0xb90bf449,   # str w9,[x2,#0xbf4]
              0x1e204060,   # fmov s0,s3               the first frame: 1/30 s
              0xb40001c1,   # cbz x1,store
              0xcb010000,   # sub x0,x0,x1
              0xd53be001,   # mrs x1,cntfrq_el0
              0x9e630000,   # ucvtf d0,x0
              0x9e630021,   # ucvtf d1,x1
              0x1e611800,   # fdiv d0,d0,d1
              0x1e624000,   # fcvt s0,d0               the frame's real time in seconds
              0x1e204061,   # fmov s1,s3
              0x7100053f,   # cmp w9,#1
              0x54000061,   # b.ne +3
              0x1e2c1001,   # fmov s1,#0.5
              0x1e210861,   # fmul s1,s3,s1            1/60 s at interval 1
              0x1e216800,   # fmaxnm s0,s0,s1
              0x1e237800,   # fminnm s0,s0,s3          at most 1/30 s
              0xbd010a00,   # store: str s0,[x16,#0x108]   the wind's delta
              0xd4200080,   # fields: adrp x0,main+0xb8f000
              0xf9406800,   # ldr x0,[x0,#0xd0]
              0xb4000260,   # cbz x0,out
              0xbd007800,   # str s0,[x0,#0x78]
              0xbd007c00,   # str s0,[x0,#0x7c]
              0xd42000a0,   # adrp x0,main+0xc22000
              0xf9416400,   # ldr x0,[x0,#0x2c8]
              0xb40001c0,   # cbz x0,out
              0xbd008000,   # str s0,[x0,#0x80]
              0x1400000c,   # b out
              0xbd010a03,   # off: str s3,[x16,#0x108]     the wind's delta as shipped
              0xb9410e09,   # ldr w9,[x16,#0x10c]
              0x34000129,   # cbz w9,out               the game's own values are in place
              0xb9010e1f,   # str wzr,[x16,#0x10c]
              0xb900061f,   # str wzr,[x16,#4]
              0x52800049,   # mov w9,#2
              0xd4200060,   # adrp x2,main+0xb74000
              0xb90bf049,   # str w9,[x2,#0xbf0]
              0xb90bf449,   # str w9,[x2,#0xbf4]
              0x1e204060,   # fmov s0,s3
              0x17ffffec,   # b fields
              0x52843408):  # out: mov w8,#0x21a0      the instruction the site held
        n = (w >> 5) & 0xffff if w & 0xffe0001f == 0xd4200000 else 0
        emit(ADRP(adrp[n][0], here(), adrp[n][1]) if n else w)
    emit(B(here(), 0x700168))
    put(0x700164, [B(0x700164, start)], 'frame step: the frame delta from real time, the present interval and the cutscene cap by the state page\'s target fps')
    put(0x747dd8, [ADRP(8, 0x747dd8, ST), 0xbd410906], 'frame step: the wind steps by the frame delta (adrp x8,state; ldr s6,[x8,#0x108])')
    put(0x6aa14c, [ADRP(8, 0x6aa14c, lit), 0xbd400100 | (lit & 0xfff) >> 2 << 10], 'frame step: the dynamic resolution\'s frame time is 1/60 s (adrp x8,literal; ldr s0,[x8,#its offset])')
    margin = here(); emit(ADRP(8, here(), ST))   # the GPU load's margin by the render scale (x8, x9 and s2 are free: the function returns after two more instructions)
    for w in (0xbd401101,   # ldr s1,[x8,#0x10]        the render scale
              0x528e1489,   # mov w9,#0x70a4
              0x72a7efa9,   # movk w9,#0x3f7d,lsl #16  0.99
              0x1e270122,   # fmov s2,w9
              0x1e222020,   # fcmp s1,s2
              0x5400012a,   # b.ge own                 1.0 (the host's default): the game's own margin
              0x1e2d1002,   # fmov s2,#0.75
              0x1e213842,   # fsub s2,s2,s1
              0x52a88909,   # mov w9,#0x44480000       800.0
              0x1e270121,   # fmov s1,w9
              0x1e210842,   # fmul s2,s2,s1
              0x1e211001,   # fmov s1,#3.0
              0x1e222821,   # fadd s1,s1,s2            3 + (0.75 - scale) x 800
              0x14000002,   # b +2
              0x1e211001):  # own: fmov s1,#3.0        the margin as shipped
        emit(w)
    emit(B(here(), 0x6aa190))
    put(0x6aa18c, [B(0x6aa18c, margin)], 'render scale: the dynamic resolution\'s GPU load margin by the state page\'s render scale (+0x10): its own at 1.0, else 3 + (0.75 - scale) x 800')
    ctx.render_scale = 1
    site(0x6debe8, 'a10100542039a052007098727f4e079409000014')   # b.ne main+0x6dec1c; mov w0,#0x1c90000; movk w0,#0xc380; bl SleepThread; b main+0x6dec1c
    wait = here()
    for w in (0x54000081,   # b.ne short               the flags are cmp w28,#2's (main+0x6debd0; adrp, add and mov since)
              0x52a03920,   # mov w0,#0x1c90000
              0x72987000,   # movk w0,#0xc380          mode 2: 30 ms, as shipped
              0x14000002,   # b sleep
              0x52a00040):  # short: mov w0,#0x20000   131,072 ns
        emit(w)
    emit(B(here(), 0x8b25f0) | 0x80000000)   # sleep: bl nn::os::SleepThread (PLT)
    emit(B(here(), 0x6dec1c))
    put(0x6debe8, [B(0x6debe8, wait)], 'task wait: a round with no pause of its own sleeps 131 µs (the frame\'s wait span a core at 96%)')

FRAME_STEP = {'f77f1559371c0ec6d50e78774ac59d95': frame_step}
