# Burnout Paradise Remastered (0100DBF01000A000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # Burnout Paradise Remastered v0 main
  '74570fa2709ed714006cd65c42a97dc6': [
    # Fl4sh9174's pchtxt mods for this build (@flag offset_shift 0x100: their addresses are the rewriter's image offsets), each original read from the extracted main:
    # 4:3 instead of 16:9 — the aspect-ratio constant (the 21:9 mod writes 2.388889 there) and the four framebuffer resolution sites (the 1920x1080 mod writes
    # mov w9,#1920 / #1080 there), set to 1280x960; plus the 21:9 mod's two NOPs (the Level of Detail mod is no longer used, see below)
    (0xd6bd18, '398ee33f', 'abaaaa3f', '4:3 mod: aspect ratio constant 16/9 → 4/3 (float 0x3FAAAAAB; the 21:9 mod writes 2.388889 here)'),
    (0x5e5f1c, '09c88052', '09a08052', '4:3 mod: handheld framebuffer width mov w9,#1600 → mov w9,#1280'),
    (0x5e5f2c, '89708052', '09788052', '4:3 mod: handheld framebuffer height mov w9,#900 → mov w9,#960'),
    (0x2b01e4, '09c88052', '09a08052', '4:3 mod: docked framebuffer width mov w9,#1600 → mov w9,#1280'),
    (0x2b01f4, '89708052', '09788052', '4:3 mod: docked framebuffer height mov w9,#900 → mov w9,#960'),
    (0x2bf594, '080300b9', '1f2003d5', '4:3 mod: the 21:9 mod\'s NOP (str w8,[x24] → nop; not a NOP in this build)'),
    (0x15a878, '000100bd', '1f2003d5', '4:3 mod: the 21:9 mod\'s NOP (str s0,[x8] → nop; not a NOP in this build)'),
    # The game's screen size (the window it creates and the space the HUD is laid out in), chosen by the operation mode at main+0x1ad70:
    # docked 1920x1080, handheld 1280x720.  The render-resolution sites above only set the picture; with the screen still 1920 wide the
    # window was 1920x1080 and the HUD, laid out in its 1920-wide space, was clipped at the right edge of the 1280-wide crop (WindowCrop 1280x960).
    (0x1ad88, 'e90f1932', '09a08052', '4:3 mod: docked screen width orr w9,wzr,#1920 → mov w9,#1280 (window + HUD layout)'),
    (0x1adac, '08878052', '08788052', '4:3 mod: docked screen height mov w8,#1080 → mov w8,#960 (window + HUD layout)'),
    (0x1ada8, '0a5a8052', '0a788052', '4:3 mod: handheld screen height mov w10,#720 → mov w10,#960 (its render is 1280x960 too)'),
    # Upstream's "Level of Detail mod" (0x5f1b50, ldr s0,[x22,#0x10] → a -15 mip LOD bias for every sampler) was here until 2026-11-11. It hid mips that
    # seemed to read black; that was the back-face winding bug (android.tsv DM_VK_FRONT=1), and with it fixed the game's own bias (0.0) samples every mip
    # correctly on the Retroid Pocket Nova, without the shimmer mip 0 everywhere gave the ground.
    # Not from the pchtxt: the boot-video FSM's state pick (cmp w8,w9 at 0xf9c40 — equal falls through to state 7, play; different branches to state 5):
    (0xf9c44, '61000054', '03000014', 'boot video skip: b.ne → b — every boot video takes the game\'s own "not enabled here" branch (state 5) instead of playing (state 7); saves ~25-30s; reversible single word'),
  ],
}
