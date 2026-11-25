# The cuts of this game's own main (NSO build cd04abd22479cf46f5e8d087f325ad28, the game's update 1.0.9): the intro video's context crashes when the Paris engine's
# per-type member table has no entry for Microsoft.Xna.Framework.Graphics.RenderTarget2D, so the member reference the game looks up is null and the game calls through it
# without a check. The crash is main+0xfafa4 (ldr x8,[x8,#0x40] with x8 = 0), in about two runs out of three; the host is not involved (the guest's assemblies and its
# reflection cache are inside main, and no metadata file is read at run time).
#
# The two cuts turn the guest's missing null check into a clean failure of the video context, so the game goes on to the menu. The good case is left bit-for-bit
# unchanged: the branch is not taken and the code runs exactly as before.
CUTS = {
  # Teenage Mutant Ninja Turtles: Shredder's Revenge 1.0.9 (v589824) main (the game's own module, the "Paris" engine's MonoGame build):
  # 1. main+0xfafa0: ldr x8,[sp,#0x10] → cbz x8, main+0xfafb4.
  #    x8 already holds the result of the member lookup (the lookup leaves it there; the probe at main+0xfaf98 printed the member descriptor in the good hits and
  #    0 in the failing one), so the load is redundant. When the lookup answered null the helper returns null (main+0xfafb4 reloads it from the stack) instead of
  #    reading +0x40 out of a null pointer.
  # 2. main+0xfd3ac: ldur x0,[x29,#-0x28] → cbz x0, main+0xfd414.
  #    x0 is the member the helper returned; with the first cut it is null in the failing case, and the branch skips the three calls through it (main+0xfefe0 at +0xfd3b0, main+0xff050 at
  #    +0xfd3fc and main+0xff0c0 at +0xfd408 - each of them calls the member's entry point with the member as the receiver, so a null member crashes inside them) and the context's
  #    "loaded" flag, so the video context fails cleanly: the flag stays 0 (its initial value at +0xfd268), the function returns at +0xfd4d4 and the game goes on to the menu.
  #    (The plan's first version of this cut branched to main+0xfd3f4, which skips only the first call: the run of 2026-11-24 06:5x then died at main+0x152c, inside the call
  #    main+0xff050 makes, so the target is the join point at +0xfd414.)
  'cd04abd22479cf46f5e8d087f325ad28': [
    (0xfafa0, 'e80b40f9', 'a80000b4', 'the intro video\'s context: the RenderTarget2D member lookup answered null → return cleanly instead of dereferencing it'),
    (0xfd3ac, 'a0835df8', '400300b4', 'the intro video\'s context: the member is null → skip the three calls through it, so the context fails cleanly'),
  ],
}
