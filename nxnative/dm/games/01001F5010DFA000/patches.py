# Pokémon Legends: Arceus (01001F5010DFA000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
#
# None for now: the game runs at its own 30 fps (present interval 2). A 60 fps patch was tried on 1.1.1 (v262144) main, build aee8f150dda1b5a838806e1a5ea6827a, and taken out:
#   (0x277d838, '280080d2', '080080d2')  display settings: present interval − 1 mov x8,#1 → mov x8,#0 (interval 1). The start-up code builds the display's settings on the stack
#                                        (main+0x277d7xx) and hands them to the display setup (main+0x29d6a64 → main+0x2aa24c4), which sets the window's present interval to that
#                                        word + 1 (main+0x2aa2788).
# Why it was taken out (AYN Thor, 2026-11-10/11): played by hand from the same save, the build with this cut runs like fast-forward (walking, running and animations about
# twice as fast) next to the build without it, which plays at the Switch's pace. The adaptive governor also swung between 60 and 40 fps there (frame work p50 16-17 ms).
CUTS = {}
