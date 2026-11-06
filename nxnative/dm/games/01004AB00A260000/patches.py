# DARK SOULS: REMASTERED (01004AB00A260000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
#
# None for now: the game runs at its own 30 fps. A 60 fps patch was tried on 1.0.3 (v196608) main, build df3766a2bb651a3e822828abf10b6f7b, and taken out:
#   (0x706fa4, 'e10f1f32', '81078052')  frame limiter target mov w1,#30 → mov w1,#60. The game presents at interval 1; its render thread (HGCommandDispatcher, main+0x8a4b28) ends each frame in a
#                                       CPU limiter, main+0x707090(this, fps): 1000 / fps ms less the time since its last call (nn::os::GetSystemTick), slept out when more than 2 ms are left; the
#                                       main thread waits for the render thread. The 30 is a constant at the only call.
#   (0x667904, '08ed46bd', '083548bd')  frame update step ldr s8,[x8,#0x6ec] (1/30) → ldr s8,[x8,#0x834] (the 1/60 rodata holds): the main thread's frame update (main+0x6a337c) takes a
#                                       fixed step, so without it everything runs twice as fast at 60 (as on the iPhone).
# Why it was taken out (Legion Y700, 2026-11-06): the frame work then sat at the limit (p50 16.7 ms, half the frames over 16.7, 0-133 of 300 dropped; the replay on a thread of its own,
# DM_NVN_ASYNC=1, did not help: the game's own CPU work is the limit), and the adaptive governor stepped to 40 fps, where a fixed 1/60 step runs the whole game at two thirds speed (a long
# run stayed at 40 for minutes). Climbing ladders also misplays at 60 (known with the community's 60 fps cheat for this build too).
CUTS = {}
