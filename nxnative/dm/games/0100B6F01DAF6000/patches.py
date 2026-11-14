# Cardfight!! Vanguard Dear Days 2 (0100B6F01DAF6000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's
# NSO build ID; CUTS as described there ("Registered patches")
CUTS = {
  # Cardfight!! Vanguard Dear Days 2 1.7.2 (v1441792) main. The update thread's frame limiter (main+0x40d120) reads the tick, adds the time since its last call to an accumulator of
  # frames (time / (1000 / fps ms), the fps a float at limiter +0xe4) and, below one frame, yields through the thunk main+0x3fa190 (b nn::os::YieldThread; its only caller) and returns;
  # the caller calls it again at once. On the host that was 3.8 million yields a second, a big core at 100% for the whole session (the Odin 2's fan at full speed): the yield back-off
  # takes the moving time in x22 for progress. The yield becomes nn::os::SleepThread of 0.52 ms through a helper in the thunk's alignment padding (main+0x3fa194, zeros, reached from
  # nowhere else). The accumulator counts the time that really passed, so a later wake-up runs the same number of updates: the game's speed does not change. Whole game on the Odin 2,
  # title screen at 60 fps: 1.27 of a core before; at 30 fps (fps_target) 0.14
  '31705d2aa09326b33eed68e95193aa3c': [
    (0x3fa194, '0000000000000000', '0001a0d2124f2514', 'frame limiter sleep helper (padding): movz x0,#0x8,lsl #16 (524,288 ns); b nn::os::SleepThread (PLT main+0xd4dde0)'),
    (0x40d240, 'd4b3ff97', 'd5b3ff97', 'frame limiter wait: bl main+0x3fa190 (YieldThread) → bl main+0x3fa194 (the sleep helper)'),
  ],
}
