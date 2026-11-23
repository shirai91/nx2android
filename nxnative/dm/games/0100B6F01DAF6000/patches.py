# Cardfight!! Vanguard Dear Days 2 (0100B6F01DAF6000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's
# NSO build ID; CUTS as described there ("Registered patches")
CUTS = {
  # Cardfight!! Vanguard Dear Days 2 1.7.2 (v1441792) main. The update thread's frame limiter (main+0x40d120) reads the tick, adds the time since its last call to an accumulator of
  # frames (time / (1000 / fps ms), the fps a float at limiter +0xe4) and, below one frame, yields through the thunk main+0x3fa190 (b nn::os::YieldThread; its only caller) and returns;
  # the caller calls it again at once. On the host that was 3.8 million yields a second, a big core at 100% for the whole session (the Odin 2's fan at full speed): the yield back-off
  # takes the moving time in x22 for progress. Title screen at 60 fps on the Odin 2: 1.27 of a core with the yield, 0.27 with a fixed 0.52 ms nn::os::SleepThread.
  # The fixed sleep still woke the limiter's thread about 2,300 times a second in the tutorial fight at 60 fps (a host sleep of 0.52 ms is 2 to 3 nanosleeps, host/hle_thread.c
  # sleep_precise); at 30 fps the presenter's cap paces the frame and the limiter rarely waits. The thunk now sleeps until the next frame is due (about 560 wake-ups a second at 60 fps): at main+0x40d240 (reached with the accumulator at or below one frame) s0 holds 1.0, s1 the frames accumulated
  # and s3 the frame's length in ns as a float (main+0x40d228 converts x20, the ns per frame of main+0x40d184); the thunk sleeps (1.0 - s1) * s3 ns. The tick base still moves by
  # the time that really passed on every call (main+0x40d45c), so the game's speed does not change. The thunk's word and the 12 bytes of alignment padding after it
  # (main+0x3fa190 to 0x3fa19f, branched to only from main+0x40d240, no pointer to it in the module) hold it.
  # The limiter's other path (a flag at limiter +0xdd, main+0x40d190: a count of frames at +0xe0) does not set s3
  '31705d2aa09326b33eed68e95193aa3c': [
    (0x3fa190, '184f2514000000000000000000000000', '0238211e4208231e4000399e114f2514',
     'frame limiter wait (thunk + padding): fsub s2,s0,s1; fmul s2,s2,s3; fcvtzu x0,s2; b nn::os::SleepThread (PLT main+0xd4dde0): sleep until the next frame is due'),
    # The resource loader's thread ("ResMgr", started at main+0x27ffe8; its job main+0x280050, called again and again by the thread loop main+0x3fc4a0) finds its request queue
    # empty and sleeps 1 ms (main+0x280654, through the ms thunk main+0x3fa180) before it looks again: about 1,000 looks and 3,100 host wake-ups a second for the whole session,
    # since a host sleep of 1 ms is 2 to 3 nanosleeps. It sleeps only when the queue is empty, so the longer poll delays only the first request after an idle spell, by at most 4 ms
    (0x280654, '20008052', '80008052', 'ResMgr idle poll: mov w0,#1 → mov w0,#4 (ms, SleepThread through main+0x3fa180)'),
  ],
}
