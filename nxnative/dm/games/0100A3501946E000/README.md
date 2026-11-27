# OCTOPATH TRAVELER II

**Experimental profile; community gameplay reported, maintainer qualification pending.** Title ID `0100A3501946E000`, package `org.nx2a.octopathtravelerii`.
The version and shader metadata come from the existing nxnative profile. After the allocator alignment fix in `914c432`, @dusk-beep reported about 30 minutes of gameplay on update 1.1.1 with no new problems reported. The [community run and log](https://github.com/shirai91/nx2android/pull/484#issuecomment-7072943355) supersede the earlier PhysX startup failure; they do not establish complete compatibility.

## Game version

The profile registers the base game with **update 1.1.1 (`v131072`)**. The three module build IDs are in [`game.tsv`](game.tsv).
Other versions have not been checked. Supply your own base game and update dumps, and your own console keys, as described in the [development guide](../../../../docs/DEVELOPMENT_GUIDE.md#port-a-game).

## Frame rate

The Android profile uses `pacing game`, preserving the game's own 30 fps limit and disabling the adaptive governor. Actual Android performance is unknown.
[`fps_locks.tsv`](fps_locks.tsv) registers this build's Unreal console variables for future testing at 60 fps. To test that target, replace the generated `DM_FPS_TARGET=0` entry in the data pack's `env.txt` with `DM_FPS_TARGET=60`; no 60 fps device result is claimed here.

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| Community phone; model not stated in this run's report | Adreno 640 | Turnip 26.99.99, Vulkan 1.3.367 (`0x6863063`) | Update 1.1.1 (`v131072`) | `914c432` | @dusk-beep: boot and about 30 minutes of gameplay, no new problems reported |

This is a community result, not a maintainer qualification. Movies, audio, save/reload, battles and traveler selection were not individually described in the report and remain unconfirmed. A maintainer run and a 1280×720 gameplay frame without overlays are still requested before adding gallery metadata.

## Options

Start with the profile defaults. It keeps docked mode and the full gamepad layout: the battle menu uses the D-pad to choose a weapon, and L / R cycle through travelers on the selection screen.
No Vulkan driver or device-specific option has been qualified. The profile has no `tested`, `readme_tag` or `readme_image` rows and is not listed in the playable-games gallery.

## Quirks and known issues

- The community run reached gameplay, but shader-table parity and the specific coverage above still need confirmation before promoting the profile.
- Upstream's Metal backend needed a larger reflection table for the traveler-selection map's 66 shader bindings. Android uses its own descriptor path (up to 96 layout bindings), so the Metal limit is not an established Android blocker; the map still needs visual validation on a device.
- Upstream also records geometry-path fixes and a rare title-screen crash. Their Android relevance is untested; see the [upstream game notes](https://github.com/seedds/nxnative/blob/d2fd78b402f92d5a83b78987e49aba22e8626719/dm/games/0100A3501946E000/README.md).
- There is no measured Android pipeline prewarm list. The shader tables are reference metadata and contain no shader binaries or game assets.
- The reported startup crash follows a PhysX allocator wrapper rejecting a pointer that is not 16-byte aligned. The host now keeps small allocations and shrinking reallocations out of Android's eight-byte tiny bin. The community retest of `914c432` passed that startup point and reported about 30 minutes of gameplay. The maintainer is also checking other Unreal allocator users before merging this shared fix.

## Profile provenance and bring-up

Reference: [seedds/nxnative commit 16f4889](https://github.com/seedds/nxnative/tree/16f4889192dd9bc6363a77b5d8b9654fe65441a2/dm/games/0100A3501946E000), which first added the 1.1.1 profile. The `shader_map.tsv`, `shader_facts.tsv`, `shader_pairs.tsv`, conflict table and `bf_want.tsv` retain that snapshot's metadata. The remaining files are adapted for nx2android's profile format; no upstream branch or runtime was merged.

The memory layout deliberately uses the original explicit 2 GB Unreal region argument, build-keyed allocator cut and observation gate, and SDK 12.3.2 node-area cuts. Current upstream instead relies on engine-wide rules that this fork does not have. The allocator and frame-limit tables are sourced from that same commit's `hle_uemalloc.c` and `hle_fpslock.c`; frame-limit pointer offsets include each console-variable object's `+0x10` value-pointer field.

The game uses Unreal Engine 4.27 with LZ4 shader records inside cooked packages in a version-11 pak, rather than a separate shader-code archive. Generic reader support accompanies this profile. Community testing has completed an APK build and a sustained gameplay session. Explicit shader-table parity, traveler selection, an opening battle and save/reload results are still requested.

Keep `status experimental` until those checks are recorded in [`android.tsv`](android.tsv). Leave `readme_tag` and `readme_image` out until the maintainer run requested in review.

## Found on this game

- [Observation gates after removed calls](../../../../docs/ARCHITECTURE.md#observation-gates-after-removed-calls): the 1.1.1 allocator cut replaces its constructor call with a NOP before the following observation gate is checked. The rewriter retains the checked original call boundary for that NOP. Community testing subsequently completed the rewrite and reached `nnMain`; the later alignment retest reached gameplay for about 30 minutes.
- [Unreal heap alignment](../../../../docs/ARCHITECTURE.md#unreal-heap-alignment): the PhysX startup trace exposed rejection of an eight-byte allocation when the host did not preserve Unreal's 16-byte minimum. The correction also covers shrinking reallocations; the subsequent community run passed startup and reached gameplay; broader Unreal-game smoke tests remain pending.
