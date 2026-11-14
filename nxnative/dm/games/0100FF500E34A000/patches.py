# Xenoblade Chronicles: Definitive Edition (0100FF500E34A000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's
# NSO build ID; CUTS as described there ("Registered patches")
CUTS = {
  # Xenoblade Chronicles: Definitive Edition 2.0.0 (v262144) main. The frame function (main+0x6015c0, called in a loop from main+0x5f02f4) waits for the engine's tasks in main+0x5eb1f0:
  # it looks the task up in the task list, and while it still runs it calls every subsystem's pump (vtable +0x48, for those with pending work at +0x648 / +0x650) and yields through
  # the PLT (three loops: one task ID at main+0x5eb424, the other two forms at main+0x5eb52c / main+0x5eb620). The pump loop leaves its list pointer and counter in x22 / x25-x27, so the
  # host's yield back-off (hle_thread.c thr_yield) takes every yield for progress and never sleeps: at the title and on loading screens the main thread yielded about 50,900 times a
  # second at 88% of a big core on the Odin 2 (yield watchdog at sdk+0x12b6988). Each yield becomes nn::os::SleepThread of 131 µs through a helper in the alignment padding after
  # main+0x5f02a0's ret (main+0x5f0314, zeros, reached from nowhere else). The wait still polls the same task state and pumps the same subsystems, only less often.
  # The GPU load the dynamic resolution reads (main+0x5bca70, a slot of the vtable at main+0xb68908): the frame's GPU time between its two timestamp counters
  # (main+0x616250 / main+0x616310, kept at gfx+0x24f0, read by main+0x603920) in µs × 1e-6 / (frame delta × vsync count) × 100, plus a margin of 3.0. Above about 100
  # the game draws its scene at 70% (896x504 instead of 1280x720; UI stays 1280x720). On the Odin 2 the first fights took 25-27 ms of GPU at the top clock
  # (680 MHz, 86-92% busy), a load of about 81, so the game never scaled. With a margin of 24 the fights draw at 896x504 with the GPU at 475-615 MHz (71-80% busy)
  # and 30 fps held (p95 33.8 ms); the title (load ~63 + 24) stays at 1280x720, at 401 MHz. A margin of 16 never scaled; 31 also scaled the lighter scenes
  '7e1df8e08d60544bbdca1e333c153c97': [
    (0x5f0314, '0000000000000000', '4000a0d2b01a0b14', 'task wait sleep helper (padding): movz x0,#0x2,lsl #16 (131,072 ns); b nn::os::SleepThread (PLT main+0x8b6dd8)'),
    (0x5eb424, 'd92e0b94', 'bc130094', 'task wait (one task): bl YieldThread (PLT main+0x8b6f88) → bl main+0x5f0314 (the sleep helper)'),
    (0x5eb52c, '972e0b94', '7a130094', 'task wait: bl YieldThread → bl main+0x5f0314 (the sleep helper)'),
    (0x5eb620, '5a2e0b94', '3d130094', 'task wait: bl YieldThread → bl main+0x5f0314 (the sleep helper)'),
    (0x5bcb14, '0110211e', '0110271e', 'GPU load for the dynamic resolution: margin fmov s1,#3.0 → #24.0 (scales down in heavy scenes)'),
  ],
}
