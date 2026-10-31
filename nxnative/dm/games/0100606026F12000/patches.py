# FINAL FANTASY RESONANCE (0100606026F12000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
import os
CUTS = {
  # FINAL FANTASY RESONANCE 1.0.1 (v65536) main: Unreal Engine 5.6.1 constructs its FMallocBinned3 in place, as Octopath Traveler 0's: nninitStartup (main+0x13370) reserves the engine region
  # (62 GB unless -SwitchVirtualAddressSpaceSize= caps it, main+0x137d0), then calls the constructor on main+0x77c6210 (bss; main+0x137f0 → main+0xae770), which reserves 38 GB of address space
  # for its pools (main+0xae860). The call is cut; the host puts its allocator's vtable in the object before any guest code runs (hle_uemalloc.c). FPlatformMemory::BaseAllocator (main+0x13c64)
  # returns the object and the thread-local storage allocator (main+0x13830 / main+0x13870) calls its Malloc / Free slots; its pool base (main+0x7c148a0) and page size (main+0x7c14800)
  # are read only by the allocator's own code (main+0xae024 to main+0xb4568)
  '84e182f72ee1da4f477804bd05b54c15': [
    (0x137f0, 'e06b0294', '1f2003d5', 'nninitStartup: bl 0xae770 (FMallocBinned3 constructor, 38 GB of address space) → nop; the host allocator is in the object already (hle_uemalloc.c)'),
  ] + ([
    # Android only: the world map's terrain stood in pillars. Unreal's GPU landscape culling (LandscapeCulling.usf: a compute pass picks the visible 4x4-quad tiles and writes the
    # instance list and indirect arguments of the terrain draw) gave tile positions that the terrain vertex shader (161d18324727dbfa) took at another LOD. With
    # landscape.EnableGPUCulling 0 (its TAutoConsoleVariable at main+0x8bafca8, set live with DM_CTL "cvari 8bafca8 0" on a POCO F7, 2026-10-31) the terrain draws uncut and right,
    # at the same 30 fps. Its registration (main+0x1ea8f50, IConsoleManager slot 1) passes the default in w2: 0 here. The culling path's own fault on Vulkan is not found yet
    (0x1ea8f48, '22008052', '02008052', 'landscape.EnableGPUCulling registration: mov w2, #1 → #0 (default off; Android only)'),
  ] if os.environ.get('DM_TARGET') == 'android' else []),
}
