# Ori and the Will of the Wisps on Android

**Experimental setup profile — known blockers remain; requires testing.** A community port attempt reached a rewriter failure; no successful game build or Android device test is established. This profile makes the exact build's metadata available to the porter; it does not establish that the game boots or plays.

**Startup crash risk:** nxnative needs a lock around Unity's shared constant-buffer table. This fork cannot apply that upstream patch, and this profile does not include it.

## Game version

- Switch 1 title ID: `01008DD013200000`.
- Source profile: **1.2.1 (`v196608`)** over the base game.
- SDK 9.4.0; Unity 2018.4.24f1 IL2CPP with Wwise, NVN rendering.
- Other builds are outside this source assessment; no Android version is claimed tested.

| Module | Build ID |
|---|---|
| `main` | `11c33c7c2dbd3c5491841f67495c7ee8` |
| `subsdk0` | `8bdabfabc6964cd9e55b7079659f51f1` |
| `sdk` | `014256a5d58180219805a7d05fba02e4` |

## Frame rate

The profile uses `pacing game`: the game's own limiter, with the adaptive governor disabled and no frame-rate override. Upstream reports a native 60 fps target on Switch/Mac; Android frame rate and pacing have not been measured.

## Tested devices

None. No APK, full shader translation, game launch, screenshots or gameplay validation were produced for this change.

## Options

- An arm64 Android device with BC texture support and the Vulkan features required by the shaders is needed. Driver compatibility is untested; the default system driver is retained.
- Start docked with the on-screen gamepad visible and rumble enabled. These are initial pause-menu defaults.
- Use the exact game version above and the player's own dump and keys. The profile is experimental even though it is selectable without generating a provisional profile.

## Quirks and known issues

- A community port of the registered `main` build stopped at `jump table 1b56540`, then at `jump table 1b574e0`. The shared rewriter now tries nxnative's tail-call proofs, the ones nxnative uses on this build, at any `br x18` it could not otherwise classify. Synthetic regressions cover these forms and unsafe variants. The instructions at `1b574e0` are not available, so a fresh full port must confirm the rewrite; later rewrite failures are also untested.
- **Unity startup race remains unfixed.** [Upstream's analysis](https://github.com/seedds/nxnative/blob/6e12b9f8471c0ea80628ef33f7ffcfe2380cada5/dm/games/01008DD013200000/README.md#startup) records concurrent main/render-thread access to a constant-buffer table and 3 crashes in 7 Mac boots without the lock. This is upstream evidence, not an Android measurement. Disabling Unity graphics jobs did not remove the race upstream.
- The upstream `registry_lock` patch for `main` build `11c33c7c2dbd3c5491841f67495c7ee8` uses `STUBS`. nx2android's `patch_nso.py::load_patches` reads only `CUTS`, `OBS` and `FRAME_STEP`. Its patch registry test also rejects unrecognized tables. The unsupported `patches.py` is deliberately omitted; a successful port command would not mean the lock was applied.
- The shader inventory includes six hull and six domain shaders. The Android graphics pipeline currently assembles vertex, fragment and geometry stages only. Whether these tessellation shaders are invoked, and the impact if they are, remain untested.
- The source profile retains the 4 GiB guest reservation, 256 MiB alias and stack regions, `sys_resource 0`, hardware application pool and `mem_used 0x9d6c000`. The remaining heap is 3.25 GiB. These are source-derived settings, not measured Android memory requirements.
- The upstream shader-prewarm loading screen can take minutes. Android startup time, movies, Wwise audio, later areas, saves and stability require testing.

## Source and profile scope

Audited against nx2android [`581ca540f7b2`](https://github.com/shirai91/nx2android/tree/581ca540f7b2214d47aee1e9be6156fc36809426). Build identity and ordinary shader tables are taken from the pinned [nxnative profile](https://github.com/seedds/nxnative/tree/6e12b9f8471c0ea80628ef33f7ffcfe2380cada5/dm/games/01008DD013200000) at `6e12b9f8471c`. The local Unity asset reader, static GLSLC-output splitter and NVN-to-Vulkan path already exist. Table reproduction from a real dump and complete shader translation have not been checked.

Only the ordinary shader map, conflict table, shader facts, shader pairs and `bf_want` inputs are included. Apple pipeline archives, supplemental `pso_pairs.tsv` and unsupported patches are not copied. The shared rewriter has a general fixed-frame tail-call recognition rule prompted by the community port failure; no runtime or shader-translator change is included. No `tested`, screenshot or tested-release metadata is supplied.

## Required testing

1. Port the exact version and retain extraction, rewrite and shader logs; check for table mismatches and missing shader inputs.
2. Test repeated fresh boots, movies, menus and the first playable scene, recording failures rather than assuming experimental means playable.
3. Watch for the documented startup race; test Swallow's Nest movement/jumping, pause menus, effects, audio and progression.
4. Exercise save/reload, later areas and a sustained session. Record device, GPU, driver, game version and route before changing the status.
