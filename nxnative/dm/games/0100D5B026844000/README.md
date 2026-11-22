# Silver Pines on Android

**Experimental setup profile — known blockers remain; requires testing.** No successful game build or Android device test is available. This profile makes the exact build's metadata available to the porter; it does not establish that the game boots or plays. The sections below follow `docs/GAME_README_TEMPLATE.md`; device facts, a screenshot and tested-release metadata must come from a real run before this profile is ready to merge.

**Rendering blocker:** the current Android shader path lacks the integer fragment outputs required by the game's rendering-layer masks. This profile does not fix that gap.

## Game version

- Switch 1 title ID: `0100D5B026844000`.
- Source profile: **1.0.0 (`v0`)**, base game.
- SDK 21.4.7; Unity 6000.3.15f1 IL2CPP with URP and FMOD, NVN rendering.
- A [community attempt with update 1.0.2 (`v131072`)](https://github.com/shirai91/nx2android/pull/519#issuecomment-7078913967) reached shader compression using a derived profile, where the OS killed the process for out-of-memory. No Android version is claimed tested.

| Module | Build ID |
|---|---|
| `main` | `91046036f8850de3ec306056fcc6ba12` |
| `subsdk0` | `92a5f1a1e8d50e4d05b2212f564bf226` |
| `sdk` | `ab3835b2a37b1916e296f27e82dd4e36` |

## Frame rate

The profile uses `pacing game`: the game's own limiter, with the adaptive governor disabled and no frame-rate override. Upstream reports a native 30 fps target on Switch/Mac; Android frame rate and pacing have not been measured.

## Tested devices

None. The community port attempt reported an OS kill at shader compression. No completed APK, game launch, screenshot or gameplay validation has been supplied; device, GPU and driver remain unknown.

## Options

- An arm64 Android device with BC texture support and the Vulkan features required by the shaders is needed. Driver compatibility is untested; the default system driver is retained.
- Start docked with the on-screen gamepad visible and rumble enabled. These are initial pause-menu defaults.
- Use the exact game version above and the player's own dump and keys. The profile is experimental even though it is selectable without generating a provisional profile.

## Quirks and known issues

- **Shader export memory:** the 1.0.2 attempt reported 66,246 split shaders (741 compute, one geometry, 32,754 pixel and 32,750 vertex), with 32,750 variation pairs. The exporter previously retained all translated SPIR-V and every compressed result. It now reads the translated files in bounded batches and writes each batch immediately. Synthetic memory regressions and bundle round trips validate this code change; the reported game build still needs a retest. The shader count alone does not establish duplicate or invalid extraction, and no shaders have been removed or capped.
- **URP integer color outputs are unsupported by the current path.** [Upstream's analysis](https://github.com/seedds/nxnative/blob/6e12b9f8471c0ea80628ef33f7ffcfe2380cada5/dm/games/0100D5B026844000/README.md#files-and-shaders) identifies 9,720 pixel shaders writing rendering-layer masks to unsigned integer attachments: location 1 in the forward pass and locations 4/5 in the deferred G-buffer. Upstream fixes this with Metal entry-point variants; that implementation does not apply to Android's Vulkan backend.
- The supplied `shader_facts.tsv` has 713 compute records and no fragment-output (`fout`) facts. `shader_vk.py` consequently uses default float outputs. The runtime's integer-image variants retype image texels, not color outputs. Correct rendering needs runtime support or independently verified per-shader output metadata; no such metadata is invented here. The street/diner may show missing geometry or fog-only rendering.
- The profile adds 1 GiB to both virtual-memory sizes: `mem_alias 0x158000000` (5.375 GiB) and `mem_reserve 0x180000000` (6 GiB). This supplies conservative headroom for the SDK's uncompressed node reservation: the upstream reduction from `0x3fe00000` to `0x3e00000` saves 960 MiB. The local SDK patch registry has no row for this exact SDK build. The 1 GiB margin is a source-derived configuration choice, not a tested Android fix or a guarantee of later allocation behavior.
- Stack capacity remains 256 MiB, heap capacity 128 MiB, `app_pool 0xCD500000` and `sys_resource 0x1000000`. `hle_svc.c` reserves address space with `MAP_NORESERVE`; reserving 6 GiB does not commit 6 GiB of physical RAM. Actual memory use still requires testing.
- VP8 logo/title movies have an existing shared libvpx decode path in the Android build. Playback, FMOD audio, saves, later scenes and stability remain untested.

## Source and profile scope

Audited against nx2android [`581ca540f7b2`](https://github.com/shirai91/nx2android/tree/581ca540f7b2214d47aee1e9be6156fc36809426). Build identity and ordinary shader tables are taken from the pinned [nxnative profile](https://github.com/seedds/nxnative/tree/6e12b9f8471c0ea80628ef33f7ffcfe2380cada5/dm/games/0100D5B026844000) at `6e12b9f8471c`. The local Unity asset reader, static GLSLC-output splitter and NVN-to-Vulkan path already exist. Table reproduction from a real dump and complete shader translation have not been checked.

Only the ordinary shader map, conflict table, shader facts, shader pairs and `bf_want` inputs are included. Apple pipeline archives, supplemental `pso_pairs.tsv` and unsupported patches are not copied. The accompanying shader export change bounds packaging memory; it does not fix the integer-output rendering blocker. No `tested`, screenshot or tested-release metadata is supplied.

## Required testing

1. Port the exact version and retain extraction, rewrite and shader logs; check for table mismatches and missing shader inputs.
2. Test repeated fresh boots, movies, menus and the first playable scene, recording failures rather than assuming experimental means playable.
3. Check the title background, motel street and diner, especially geometry and rendering-layer masks; watch for address-region allocation failures.
4. Exercise save/reload, later areas and a sustained session. Record device, GPU, driver, game version and route before changing the status.
