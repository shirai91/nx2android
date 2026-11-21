# OCTOPATH TRAVELER II

**Experimental profile; Android support is not validated.** Title ID `0100A3501946E000`, package `org.nx2a.octopathtravelerii`.
The version and shader metadata come from the existing nxnative profile. Community testing has completed the rewrite, built and installed an APK, and reached `nnMain`; startup then crashed in PhysX. Successful gameplay remains unverified.

## Game version

The profile registers the base game with **update 1.1.1 (`v131072`)**. The three module build IDs are in [`game.tsv`](game.tsv).
Other versions have not been checked. Supply your own base game and update dumps, and your own console keys, as described in the [development guide](../../../../docs/DEVELOPMENT_GUIDE.md#port-a-game).

## Frame rate

The Android profile uses `pacing game`, preserving the game's own 30 fps limit and disabling the adaptive governor. Actual Android performance is unknown.
[`fps_locks.tsv`](fps_locks.tsv) registers this build's Unreal console variables for future testing at 60 fps. To test that target, replace the generated `DM_FPS_TARGET=0` entry in the data pack's `env.txt` with `DM_FPS_TARGET=60`; no 60 fps device result is claimed here.

## Tested devices

No device is qualified. Community startup testing exposed the allocator issue below; a retest with the alignment correction is still needed. Boot, movies, audio, rendering, controls, frame pacing and saves still need Android validation.

## Options

Start with the profile defaults. It keeps docked mode and the full gamepad layout: the battle menu uses the D-pad to choose a weapon, and L / R cycle through travelers on the selection screen.
No Vulkan driver or device-specific option has been qualified. The profile has no `tested`, `readme_tag` or `readme_image` rows and is not listed in the playable-games gallery.

## Quirks and known issues

- This is preparation for bring-up, not a report of a working port. Build the APK and compare the generated shader tables with this profile before installing or promoting its status.
- Upstream's Metal backend needed a larger reflection table for the traveler-selection map's 66 shader bindings. Android uses its own descriptor path (up to 96 layout bindings), so the Metal limit is not an established Android blocker; the map still needs visual validation on a device.
- Upstream also records geometry-path fixes and a rare title-screen crash. Their Android relevance is untested; see the [upstream game notes](https://github.com/seedds/nxnative/blob/d2fd78b402f92d5a83b78987e49aba22e8626719/dm/games/0100A3501946E000/README.md).
- There is no measured Android pipeline prewarm list. The shader tables are reference metadata and contain no shader binaries or game assets.
- The reported startup crash follows a PhysX allocator wrapper rejecting a pointer that is not 16-byte aligned. The host now keeps small allocations and shrinking reallocations out of Android's eight-byte tiny bin. Whether this resolves the device crash is awaiting a community retest.

## Profile provenance and bring-up

Reference: [seedds/nxnative commit 16f4889](https://github.com/seedds/nxnative/tree/16f4889192dd9bc6363a77b5d8b9654fe65441a2/dm/games/0100A3501946E000), which first added the 1.1.1 profile. The `shader_map.tsv`, `shader_facts.tsv`, `shader_pairs.tsv`, conflict table and `bf_want.tsv` retain that snapshot's metadata. The remaining files are adapted for nx2android's profile format; no upstream branch or runtime was merged.

The memory layout deliberately uses the original explicit 2 GB Unreal region argument, build-keyed allocator cut and observation gate, and SDK 12.3.2 node-area cuts. Current upstream instead relies on engine-wide rules that this fork does not have. The allocator and frame-limit tables are sourced from that same commit's `hle_uemalloc.c` and `hle_fpslock.c`; frame-limit pointer offsets include each console-variable object's `+0x10` value-pointer field.

The game uses Unreal Engine 4.27 with LZ4 shader records inside cooked packages in a version-11 pak, rather than a separate shader-code archive. Generic reader support accompanies this profile. It still needs a full split against the real update, shader translation, an APK build and a device run through traveler selection, an opening battle, a save/reload and a sustained play session.

Keep `status experimental` until those checks are recorded in [`android.tsv`](android.tsv). Add a device-captured gameplay image and the tested-version tag only after an actual device run.

## Found on this game

- [Observation gates after removed calls](../../../../docs/ARCHITECTURE.md#observation-gates-after-removed-calls): the 1.1.1 allocator cut replaces its constructor call with a NOP before the following observation gate is checked. The rewriter retains the checked original call boundary for that NOP. Community testing subsequently completed the rewrite and reached `nnMain`; Android gameplay remains unverified.
- [Unreal heap alignment](../../../../docs/ARCHITECTURE.md#unreal-heap-alignment): the PhysX startup trace exposed rejection of an eight-byte allocation when the host did not preserve Unreal's 16-byte minimum. The correction also covers shrinking reallocations; device confirmation remains outstanding.
