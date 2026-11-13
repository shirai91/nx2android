# Registered 1.1.1 cut referenced from seedds/nxnative 3084f7a205bcb3a9cb74cf26180dabcc01c7abca.
# Taiko Risshiden V DX (0100346017304000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # Taiko Risshiden V DX 1.1.1 (v131072) main: KTGL's frame end (main+0x443e8c: the present wrapper, which presents and acquires the next texture, then the wait) waits until
  # N / 60 s have passed since the frame's start tick, polling nn::os::GetSystemTick and sleeping 2 ms per round (main+0x4c9f24, SleepThread). The start tick is read at
  # main+0x443d44, after the scene's update (the frame object's vtable +0x10, main+0x443cec), so every frame takes N / 60 s plus the time from the last poll to the next start,
  # rounded up to the 2 ms rounds: never one vsync, on any hardware (the Switch's double-buffered acquire only moves where the rounds start; a model of both gives 54 fps). That
  # is the game's own code, not the host: SleepThread and the tick are exact there. The Mac ran 55 fps in the menus and the town (frame time p5 18.15 / p50 18.28 ms), every 11th
  # frame on screen for two vsyncs. N, the int at GOT 0xa1c088 (data 0x9ec764), is the game's vsyncs per frame: 1, and 2 in CPrivateBattleWindow / CPrivateBattleTutorialWindow
  # (main+0xf02d0, main+0xec4cc; back to 1 at main+0xf04f4), whose code counts 60 / N frames a second. The game presents at interval 1 in both, so with N = 1 the host's present
  # pacing (the acquire waits) holds the frame to one vsync, and the wait's threshold becomes 0 (the loop returns after its first tick, which it still stores at +0x38). With N = 2
  # nothing else holds the frame two vsyncs, so the wait stays the game's (N forced to 2 at the title: 28.9 fps, as without a cut; the earlier cut, b.hs → b at main+0x443fd4,
  # presented 60). In the town then: 60.0 fps, frame time p50 16.66 / p95 17.08 ms; the main thread waits in the acquire instead of nine sleeps a frame (wakeups 2,050 → 300
  # a second). The opening movie, paced by its own player, was at 60 before
  'f5ab74b9d45f955de2dc91da8e039c57': [
    (0x443fac, '087c149be9e301b2291191f2087dc99b14fd45d3', '087c149b890780520809c99a9f060071f403889a',
     'frame limiter threshold: N × tick frequency / 60 (umulh by 1/60 → udiv by 60), then 0 when N = 1 (cmp w20, #1; csel x20, xzr, x8, eq): one vsync a frame is the present pacing\'s; N = 2 keeps the 2 ms rounds'),
  ],
}
