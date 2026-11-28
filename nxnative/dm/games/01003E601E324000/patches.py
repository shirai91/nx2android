# DRAGON QUEST III HD-2D Remake (01003E601E324000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a
# module's NSO build ID; CUTS and OBS as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # DRAGON QUEST III HD-2D Remake 1.1.2 (v262144) main: Unreal Engine 4.27's FMallocBinned3 takes 37 GB of the engine's address region, which the profile caps at 2 GB (game_args
  # -SwitchVirtualAddressSpaceSize=2048). GCreateMalloc (main+0xf5cf8c) mallocs 0x1c38 bytes, constructs them (main+0xf5cfa4 → main+0xfb7980) and returns the object; the constructor
  # call is cut, and observation gate OBS_UE_MALLOC_X0 (OBS below) puts the host allocator's vtable into the new object in its caller (dm/host/hle_uemalloc.c). Found by upstream's
  # FMallocBinned3 matcher (dm/tools/ue_malloc.py): the name string, its getter, the vtable (main+0x4fd5040, getter slot 19 = UE 4.26 / 4.27) and the one call of its constructor
  'a6bd61cfb142e663f59f4d92b4ed0474': [
    (0xf5cfa4, '776a0194', '1f2003d5', 'GCreateMalloc: bl 0xfb7980 (FMallocBinned3 constructor, 37 GB of address space) → nop; the host allocator\'s vtable is put in at the gate after it (hle_uemalloc.c)'),
  ],
}
OBS = {
  # DRAGON QUEST III HD-2D Remake 1.1.2 main: GCreateMalloc is a leaf (malloc, construct, return), so the gate sits in its only caller, the store of GMalloc right after it returned:
  # x0 = the new allocator object (Unreal's GMalloc), used through its vtable three instructions later (hle_uemalloc.c, OBS_UE_MALLOC_X0)
  'a6bd61cfb142e663f59f4d92b4ed0474': [
    (0xfc6e1c, 22, 'f90002a0', 'GCreateMalloc caller: the allocator object is in x0 at its store into GMalloc, before anything uses it: the host puts its vtable in; orig str x0, [x21]'),
  ],
}
