# Pokémon Sword (0100ABF008968000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # 1.3.2 (v458752) main. With a save, the title screen builds the player's character in their outfit (sd9010_title.bseq, model a_pl0110). The dress-up manager's
  # function at main+0xcaaed0 (under its mutex at +0xade8) hands each outfit part's handle from the slot table at +0xad78 to main+0x64f970, which reads the handle's
  # object at main+0x64fa78 (ldr x8, [x8, #0x58]) without a check. The slots are filled by main+0xcab9a0 (called 14 times from main+0xca8a20) as the parts load: on the
  # Switch they are always there by then; on the Odin 2 (2026-11-13) a slot was still empty in 2 of 12 cold boots: SIGSEGV at main+0x64fa78 (address 0x58), 1.5 s in.
  # Horizon's same-core preemption (DM_PREEMPT=1, the core_preempt key) did not help (3 of 8 boots). Each call site already skips the handle's reference count for an empty slot
  # (cbz x0); its branch now goes past the call too, to the reload of the slot after it (which finds it empty and skips the release): a part that has not loaded yet
  # is not applied this time, instead of the null read
  'a3b75bcd3311385aeed67fbeeb79cbb7': [
    (0xcab11c, '800000b4', '000100b4', 'dress-up slot (part index + 7) empty: skip main+0x64f970 (cbz x0 → main+0xcab13c instead of main+0xcab12c)'),
    (0xcab168, '800000b4', '000100b4', 'dress-up slot (part index) empty: skip main+0x64f970 (cbz x0 → main+0xcab188 instead of main+0xcab178)'),
    (0xcab1f4, '800000b4', '000100b4', 'dress-up slot (part index) empty: skip main+0x64f970 (cbz x0 → main+0xcab214 instead of main+0xcab204); the crash of 2026-11-13'),
    (0xcab244, '800000b4', '000100b4', 'dress-up slot (part index + 7) empty: skip main+0x64f970 (cbz x0 → main+0xcab264 instead of main+0xcab254)'),
  ],
}
