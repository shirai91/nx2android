# Need for Speed Hot Pursuit Remastered (010029B0118E8000): the rewriter's patches of this game's own builds (dm/tools/patch_nso.py "Registered patches")
CUTS = {
  # NFS Hot Pursuit Remastered 1.0.3 (v196608) main, Frostbite: native 4:3 for a 4:3 screen (Retroid Pocket Nova). The window's three textures and the game's
  # display-size object (0xf00340) are set to 1280x960 in both modes (the values main+0x7ebad0 stores for nvnWindowBuilderSetTextures, and the ones
  # main+0x8180 / main+0x2e00 pick from nn::oe::GetOperationMode); the engine's view and HUD follow that size, so this is a real 4:3 frame, not a stretch or a
  # crop. Local, not for every player.
  # The frame limiter: the game's 60 (ticks) gives the stock 30 fps; the community 60FPS mod's 120 gives 60 fps (Fl4sh_#9174 / pacoa-kdbg), with the host
  # presenting every display frame (present_interval display in game.tsv).
  '799d1061182c1302e08e706b84ce76e5': [
    # the presentation swap chain: w | h<<32 stored as the size of the three window textures (main+0x7ebafc / 0x7ebbf4 / 0x7ebcd8)
    (0x7ebae8, '16f080d2', '16a080d2', 'swap chain width: mov x22,#1920 → #1280'),
    (0x7ebaec, '1687c0f2', '1678c0f2', 'swap chain height: movk x22,#1080,lsl #32 → #960'),
    # the display-size object at 0xf00340: constructor, startup (from oe::GetOperationMode) and the operation-mode notification handler
    (0x7f58, 'e80f1932', '08a08052', 'display size ctor width: 1920 → 1280'),
    (0x7f6c, '08878052', '08788052', 'display size ctor height: 1080 → 960'),
    (0x8198, 'e90f1932', '09a08052', 'display size init (docked) width: 1920 → 1280'),
    (0x81a4, '095a8052', '09788052', 'display size init (handheld) height: 720 → 960'),
    (0x81ac, '0a878052', '0a788052', 'display size init (docked) height: 1080 → 960'),
    (0x2e4c, '1f011e71', '1f011471', 'mode-change handler: cmp w8,#1920 → #1280'),
    (0x2e54, 'e80f1932', '08a08052', 'mode-change handler (docked) width: 1920 → 1280'),
    (0x2e68, '1fe11071', '1f010f71', 'mode-change handler: cmp h,#1080 → #960'),
    (0x2e70, '08878052', '08788052', 'mode-change handler (docked) height: 1080 → 960'),
    (0x2ea4, '1f410b71', '1f010f71', 'mode-change handler: cmp h,#720 → #960'),
    (0x2eac, '085a8052', '08788052', 'mode-change handler (handheld) height: 720 → #960'),
    # the engine's own aspect constant (the address pacoa/Fl4sh's ultrawide mod patches): 4:3, so the UI safe area and the 3D view are laid out for the 1280x960 window
    (0xb78de8, '398ee33f', 'abaaaa3f', 'aspect constant: 16:9 (1.7778) → 4:3 (1.3333)'),
    # frame limiter: 60 ticks (30 fps) → 120 (60 fps)
    (0x2b7de0, 'e00f1e32', '000f8052', 'frame limiter: mov w0,#0x3c (30 fps) → #0x78 (60 fps)'),
  ],
}
