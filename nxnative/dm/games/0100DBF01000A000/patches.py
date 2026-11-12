# Burnout Paradise Remastered (0100DBF01000A000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # Burnout Paradise Remastered v0 main
  '74570fa2709ed714006cd65c42a97dc6': [
    # The boot-video FSM's state pick (cmp w8,w9 at 0xf9c40 — equal falls through to state 7, play; different branches to state 5):
    (0xf9c44, '61000054', '03000014', 'boot video skip: b.ne → b — every boot video takes the game\'s own "not enabled here" branch (state 5) instead of playing (state 7); saves ~25-30s; reversible single word'),
  ],
}
