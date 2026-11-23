# Fire Emblem: Three Houses (010055D009F78000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # Fire Emblem: Three Houses 1.2.0 (v327680) main: 60 fps. KTGL runs the game on the main thread at 60 updates a second whatever it shows: each update steps by
  # 1/fps with fps = 60.0 (main+0x1a38f08, set at main+0x50e114; dt at main+0x510010), and the frame end (main+0x50f680) waits out 60/fps ticks of 1/59.94 s in a yield loop
  # (main+0x5be430). The render thread (main+0x50f8c0) draws the latest update and presents with the interval of the render mode (frame +0xbb84, set at main+0x50f3d0:
  # 0 = 1080p docked, 2 = 720p handheld; 3 / 5 their two-view variants; 1 and 4 are 720p / 1080p modes at interval 1 that nothing selects). Measured on the Mac: 59.2 updates
  # and 29.8 presents a second. The per-mode interval table (int at main+0xd13de0, read by main+0x4b0420 for the present and by the 6-tick fade counter in main+0x4b7530;
  # float at main+0xd13e40, the per-frame scroll step in main+0x4c2b30) becomes 1 for every mode, as in modes 1 and 4: every update is shown and the render thread's
  # own steps stay at the same speed. Resolutions stay per mode
  '89048449ba238c8cf565518b83bf02d3': [
    (0xd13de0, '020000000100000002000000020000000100000002000000', '010000000100000001000000010000000100000001000000', 'per-render-mode present interval table {2,1,2,2,1,2} → all 1 (60 fps; the game logic already runs at 60 updates a second)'),
    (0xd13e40, '000000400000803f00000040000000400000803f00000040', '0000803f0000803f0000803f0000803f0000803f0000803f', 'per-render-mode interval as float {2.0,1.0,2.0,2.0,1.0,2.0} → all 1.0 (render-thread per-frame steps)'),
    # The render thread still presents with interval 2 while either of two flags is set: main+0x21c9a03, a full-screen image drawn before the frame (main+0x4b4908: the opening movie),
    # and frame +0xc55f / +0xc560 (from the object at app +0x40: the character creation screen after the movie, and so on). In the engine's own 60 fps modes this traded 60 fps for
    # resolution in those scenes (main+0x50f7d0 drops 720p mode 1 to 1080p mode 0); here it would show them at 30 with the render thread's own steps at half speed. Both are ignored
    (0x50fb7c, '2801082a', 'e8031f2a', 'render thread forced interval 2: orr w8,w9,w8 (either flag) → mov w8,wzr (the mode\'s interval, 1)'),
    # The frame limiter's wait (main+0x5be430): until 60/fps ticks of 1/59.94 s (the double at main+0xcb91a8) have passed since the last frame end, it yields and reads the tick.
    # Three things in it broke the 60 Hz display pacing on Android, each a frame shown for three vsyncs (a resync):
    # - the yield loop is a busy loop on the host (2 ms spinning on a performance core, then oversleeps of up to 250 µs): frames of 16.8 ms, and 1.5 W of CPU on the Mac;
    # - the tick of 1/59.94 s (the Switch's output rate): each frame came 16 µs late against the 60.00 Hz cadence, a resync about every 100 frames;
    # - the next frame counts from this frame's real end, so every wake-up after the target is added to the frame clock: with a sleep and a 2 ms yield loop the frames
    #   ran at about 16.668 ms, 100 µs a second behind the display (in docked battle, the present margin fell from 3 ms to 0 in 30-60 s, then a resync: 19 in 20 minutes).
    # The function is now: P = tick frequency × (60 / fps) / 60 (exactly 1/60 s per vsync); target = the last planned frame end + P; sleep (nn::os::SleepThread) while more
    # than 2 ms are left, until 2 ms before the target (a sleep on Android sometimes wakes 1-2 ms late), then yield; store the target, not the real end, as the frame end.
    # A frame more than one frame late (a load) starts the plan again from its real end, so the frames after it do not run without a wait. The double at main+0xcb91a8 is no longer read
    (0x5be430, 'f70f1cf8f65701a9f44f02a9fd7b03a9fdc3009114d092521413a072d9f7149495a700d0b5fe40f9a00200f993a700d0730241f9680240f996a700d0d6fa40f9080008cb087d149bc90240f93f0108eb280300548aa700b04a2945f9400140bd0a00381eeb0f1e326a0dca1a4a7d349b4001639eca3700f041d540fd0018611e1700799e0809c99a1f0117eb4201005420fa1494bbf71494680240f9080008cb087d149bc90240f93f0108eba00200f9a9feff546bf81494c00200f9b1f71494600200f9fd7b43a9f44f42a9f65741a9f70744f8c0035fd6',
               'fd7bbda9f35301a9f55b02a9fd03009193a700d0730241f996a700d0d6fa40f990f81494c00200f98aa700b04a2945f9400140bd0a00381e8b0780526a0dca1a147c0a9b8b0780d2940acb9a750240f9b502148bcbf7149488a700d008fd40f9000100f9a10200ebcd010054094099d24973a7f2217c099bc90240f92108c99a099090d2c903a0f2200009eb6900005490f71494f0ffff171efa1494eeffff17020015cb5f0014eb1580959a750200f9f55b42a9f35341a9fd7bc3a8c0035fd6000000000000000000000000000000000000000000000000',
     'frame limiter wait: a wait for planned frame ends at exactly 60/fps vsyncs of 1/60 s each (sleep, then yield for the last 2 ms), see above'),
    # The render thread waits for the next update with KTGL's event wait (main+0xaa90e0, timeout in µs: × 1000 → nn::os::TimedWaitEvent) and passes 1/fps × 1000 = 16, one frame in ms,
    # so it polls every 16 µs: at 60 fps it draws in about 3 ms and then polled for the rest of the frame (42,000–53,000 wakeups a second on the Mac, against 1,200 at 30 fps, where
    # it waited in the present instead). The other four callers wait without a timeout. The timeout becomes 1000/fps × 1000 = 16,666 µs (s8 = 1000.0, loaded first)
    (0x50f8e8, '01102e1e', '483d0090', 'render thread wait timeout: fmov s1,#1.0 → adrp x8,#0xcb7000'),
    (0x50f8ec, '2018201e', '08514bbd', 'render thread wait timeout: fdiv s0,s1,s0 → ldr s8,[x8,#0xb50] (1000.0)'),
    (0x50f8f0, '483d0090', '0019201e', 'render thread wait timeout: adrp x8,#0xcb7000 → fdiv s0,s8,s0 (1000/fps; the fmul by s8 that follows makes it µs)'),
    (0x50f8f4, '08514bbd', '1f2003d5', 'render thread wait timeout: ldr s8,[x8,#0xb50] → nop (moved up)'),
  ],
}
