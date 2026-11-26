# DRAGON QUEST VII Reimagined, Asian release (0100505021440000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies
# those of a module's NSO build ID; CUTS and OBS as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # DRAGON QUEST VII Reimagined 1.1.0 main: Unreal Engine 4.27's FMallocBinned3 takes 37 GB of the engine's address region. GCreateMalloc (main+0x3275590) mallocs 0x1c38 bytes, constructs
  # them (main+0x32755bc → main+0x291714) and stores GMalloc (main+0x77767e0); the constructor call is cut, and observation gate OBS_UE_MALLOC (OBS below) puts the host allocator's vtable
  # into the new object, which is still in x19 (dm/host/hle_uemalloc.c)
  'ab06020960edd8285b7ff5fad2e3224b': [
    (0x32755bc, '56704097', '1f2003d5', 'GCreateMalloc: bl 0x291714 (FMallocBinned3 constructor, 37 GB of address space) → nop; the host allocator\'s vtable is put in at the gate after it (hle_uemalloc.c)'),
  ],
}
OBS = {
  # DRAGON QUEST VII Reimagined 1.1.0 main: right after the cut FMallocBinned3 constructor call (CUTS above), x19 = the new allocator object; the host writes its vtable there (hle_uemalloc.c)
  'ab06020960edd8285b7ff5fad2e3224b': [
    (0x32755c4, 21, 'aa1303e0', 'GCreateMalloc: the allocator object is in x19, before it is used: the host puts its vtable in; orig mov x0, x19'),
  ],
}