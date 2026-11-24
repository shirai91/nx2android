# Silver Pines on Android

**Experimental profile. A community tester plays update 1.0.2 on Android; the registered 1.0.0 build is untested.** dusk-beep ported update 1.0.2 through a porter-derived profile, with the bounded shader export from #562, and played it on a POCO X3 Pro. This profile still registers only 1.0.0, because 1.0.2's own shader tables have not been supplied yet.

## Game version

- Switch 1 title ID: `0100D5B026844000`.
- Registered build: **1.0.0 (`v0`)**, base game. It has not been run on Android.
- Community-tested: **update 1.0.2 (`v131072`)**, through a derived profile. Its module build IDs, from the tester's derived `game.tsv`, are `main` `fde81dd4a0bef165765435c535342489`, `subsdk0` `92a5f1a1e8d50e4d05b2212f564bf226` and `sdk` `ab3835b2a37b1916e296f27e82dd4e36`.
- SDK 21.4.7; Unity 6000.3.15f1 IL2CPP with URP and FMOD, NVN rendering.

| Module (1.0.0) | Build ID |
|---|---|
| `main` | `91046036f8850de3ec306056fcc6ba12` |
| `subsdk0` | `92a5f1a1e8d50e4d05b2212f564bf226` |
| `sdk` | `ab3835b2a37b1916e296f27e82dd4e36` |

## Frame rate

- The game targets 30 fps: present interval 2 on a 60 Hz display. The profile uses `pacing game`, the game's own limiter with the adaptive governor off.
- In the 2026-11-24 run, the tester saw drops to about 15 fps, in the same places on every run and in many places through the game. The log has 2,879 long frames among 43,073 frames in 28 minutes. The cause is not known.

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| POCO X3 Pro (Snapdragon 860, 6 GB, Android 13), community test, 2026-11-23 and 2026-11-24 | Adreno 640 | Turnip, Mesa 27.0.0-devel (git-2648b2059f) | 1.0.2 (`v131072`), derived profile | this PR's profile with #562; exact commit not reported | plays; no rendering errors seen on the street or in the diner; frequent drops to about 15 fps |

Results: [first run](https://github.com/shirai91/nx2android/pull/519#issuecomment-7089754345), [retest](https://github.com/shirai91/nx2android/pull/519#issuecomment-7126204142).

## Options

- An arm64 Android device with BC texture support. The only device run used Turnip on Adreno 640; the system driver is untested for this game.
- Start docked with the on-screen gamepad visible and rumble enabled. These are initial pause-menu defaults.
- Update 1.0.2 currently takes the porter's derived-profile path, which adds alias headroom for that build. The registered 1.0.0 layout has not been checked with 1.0.2.

## Quirks and known issues

- **Shader export memory** was the first port's failure: the OS killed the port while compressing 1.0.2's shaders. #562 bounds that memory, and the retest loads 66,250 compressed shaders.
- **URP integer colour outputs.** [Upstream's analysis](https://github.com/seedds/nxnative/blob/6e12b9f8471c0ea80628ef33f7ffcfe2380cada5/dm/games/0100D5B026844000/README.md#files-and-shaders) finds 9,720 pixel shaders writing rendering-layer masks to unsigned integer attachments. The Android path uses default float outputs for them. The tester saw no rendering errors on the street or in the diner, so this has no visible effect in the scenes played, but later scenes are unchecked.
- **Memory layout.** The derived 1.0.2 profile uses `mem_reserve 0x380000000` and `mem_alias 0x358000000` (13.38 GiB alias region), where this profile has `0x180000000` and `0x158000000`. Registering 1.0.2 must keep the larger layout.
- Stack 256 MiB, heap 128 MiB, `app_pool 0xCD500000`, `sys_resource 0x1000000`. Address space is reserved with `MAP_NORESERVE`, so the virtual sizes do not commit physical RAM.
- The run's log shows VP8 movie decoding, audio callbacks and 21 save commits. Save reload after a restart has not been reported.

## Source and profile scope

The registered 1.0.0 build identity and shader tables come from the pinned [nxnative profile](https://github.com/seedds/nxnative/tree/6e12b9f8471c0ea80628ef33f7ffcfe2380cada5/dm/games/0100D5B026844000) at `6e12b9f8471c`. Only the ordinary shader map, conflict table, shader facts, shader pairs and `bf_want` inputs are included. Apple pipeline archives, `pso_pairs.tsv` and unsupported patches are not copied.

## Still needed

1. 1.0.2's derived profile tables (`shader_map.tsv`, `shader_map.tsv.conflicts.tsv`, `shader_facts.tsv`, `shader_pairs.tsv`, `bf_want.tsv`) from `0100D5B026844000_build/profile-derived/0100D5B026844000/`, so that 1.0.2's build row, tables and memory layout can be registered together.
2. The nx2android commit that was ported.
3. A gameplay frame captured on the device with the overlay off, for `docs/images/silver-pines.jpg`, then the `readme_image` / `readme_tag` rows and `python docs/gen_tables.py`.
