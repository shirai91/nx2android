# Pokémon Legends: Arceus (01001F5010DFA000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
#
# None for now: the game runs at its own 30 fps (present interval 2). A 60 fps patch was tried on 1.1.1 (v262144) main, build aee8f150dda1b5a838806e1a5ea6827a, and taken out:
#   (0x277d838, '280080d2', '080080d2')  display settings: present interval − 1 mov x8,#1 → mov x8,#0 (interval 1). The start-up code builds the display's settings on the stack
#                                        (main+0x277d7xx) and hands them to the display setup (main+0x29d6a64 → main+0x2aa24c4), which sets the window's present interval to that
#                                        word + 1 (main+0x2aa2788).
# Why it was taken out (AYN Thor, 2026-11-10): walking ran far too fast at 60 fps, so at least the player's movement takes a fixed step per frame; the adaptive governor also swung
# between 60 and 40 fps there (frame work p50 16-17 ms), which would change the speed from moment to moment. The Y700 check (the same walk at 60 and pinned at 30) had missed it.
CUTS = {}
