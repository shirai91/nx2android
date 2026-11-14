# FINAL FANTASY RESONANCE, Asian release (0100B8A026F14000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and
# applies those of a module's NSO build ID; CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # FINAL FANTASY RESONANCE 1.0.1 (v65536) main, the Korean / Chinese release: the same Unreal Engine 5.6.1 build at the same offsets as the JP profile
  # (dm/games/0100606026F12000/patches.py). nninitStartup (main+0x13370) reserves the engine region (62 GB unless -SwitchVirtualAddressSpaceSize= caps it, main+0x137d0),
  # then calls the FMallocBinned3 constructor on main+0x77c6210 (bss; main+0x137f0 → main+0xae770), which reserves 38 GB of address space for its pools (main+0xae860).
  # The call is cut; the host puts its allocator's vtable in the object before any guest code runs (hle_uemalloc.c). The original bytes are asserted, so a build that
  # does not hold this call stops the rewrite instead of being patched at an address it was not found in.
  'b7f965b9d7227db8e29f04e84ea7331b': [
    (0x137f0, 'e06b0294', '1f2003d5', 'nninitStartup: bl 0xae770 (FMallocBinned3 constructor, 38 GB of address space) → nop; the host allocator is in the object already (hle_uemalloc.c)'),
  ],
}
