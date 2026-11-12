# Pokémon Shield (01008DB008C2C000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # 1.3.2 (v458752) main: the same cuts as Pokémon Sword 1.3.2's (0100ABF008968000/patches.py, where the title screen's race is described), carried over with
  # dm/tools/nso_fnmap.py: the dress-up function is Sword's main+0xcaaed0 at main+0xcaaf00 here (an exact match), so each call site is 0x30 later; the null read
  # stays at main+0x64fa78. A part that has not loaded yet is not applied this time, instead of the null read
  'a16802625e7826bf83b6f9708e475b91': [
    (0xcab14c, '800000b4', '000100b4', 'dress-up slot (part index + 7) empty: skip main+0x64f970 (cbz x0 → main+0xcab16c instead of main+0xcab15c)'),
    (0xcab198, '800000b4', '000100b4', 'dress-up slot (part index) empty: skip main+0x64f970 (cbz x0 → main+0xcab1b8 instead of main+0xcab1a8)'),
    (0xcab224, '800000b4', '000100b4', 'dress-up slot (part index) empty: skip main+0x64f970 (cbz x0 → main+0xcab244 instead of main+0xcab234)'),
    (0xcab274, '800000b4', '000100b4', 'dress-up slot (part index + 7) empty: skip main+0x64f970 (cbz x0 → main+0xcab294 instead of main+0xcab284)'),
  ],
}
