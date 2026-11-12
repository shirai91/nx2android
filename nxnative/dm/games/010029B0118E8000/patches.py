# Need for Speed Hot Pursuit Remastered (010029B0118E8000): the rewriter's patches of this game's own builds (dm/tools/patch_nso.py "Registered patches")
CUTS = {
  # NFS Hot Pursuit Remastered 1.0.3 (v196608) main: the frame limiter, the game's 60 ticks (30 fps) → the community 60FPS mod's 120 (60 fps;
  # Fl4sh_#9174 / pacoa-kdbg), with the host presenting every display frame (present_interval display in game.tsv)
  '799d1061182c1302e08e706b84ce76e5': [
    (0x2b7de0, 'e00f1e32', '000f8052', 'frame limiter: mov w0,#0x3c (30 fps) → #0x78 (60 fps)'),
  ],
}
