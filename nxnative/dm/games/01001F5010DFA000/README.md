<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->
# Pokémon Legends: Arceus

![Pokémon Legends: Arceus](../../../../docs/images/pokemon-legends-arceus.jpg)

| | |
|---|---|
| Status | **playable** |
| Title ID | `01001F5010DFA000` |
| Tested with nx2android | `v0.4.0` |
| Tested build and device | v262144 AYN Thor (Snapdragon 8 Gen 2, Adreno 740, Android 13) |
| App package | `org.nx2a.plarceus` |

Read the profile's porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list](../../../../README.md#games).
<!-- end of the game facts -->

## Game version

- Update 1.1.1.
- The profile was checked with `v262144` (the `tested` row of android.tsv).

## Frame rate

- 60 fps: `patches.py` carries [PLARecomp](https://github.com/artesanalintelligence/PLARecomp)'s corrected 60 fps patch list for this build (`recomp/patches/60fps/pla-111-60fps-fixed.json`, 61 sites, every original byte asserted against 1.1.1) and the profile sets `fps_target 60`. It moves the engine's step from 1/30 to 1/60 where it is held (timers, movement, gravity, velocity integration, AI prediction, the ride, the sky), the frame clock's ns-per-frame table from 33333334 to 16666667, the presentation interval to 1, and sends six divisions by 30 through a cave in the text's padding.
- Why the step has to be moved: the engine advances one step per update and builds that step as a `movz`/`movk` immediate, so there is no constant in the image to scale — the presentation-interval cut alone doubled walking speed (PR #219), which is what took 60 fps out then.
- Logic runs at the Switch's speed only while the game holds 60 fps: below it the fixed 1/60 step makes the game slower, not faster. On a device that cannot hold 60 the adaptive governor's drops will show as slow motion.
- Measured on the 2410DPN6CC (Snapdragon 8 Elite, Adreno 830, Android 16) with nx2android `v0.4.0` (the reason it is not `main` is below): 60 fps held in the field with `DM_OPMODE=0` (handheld) and `DM_GPU_TURBO=1` — frame work p50 8.0–8.5 ms, 0 dropped of 300 frames — with the in-game menu and the tutorial in Chinese. Without those two switches the same build has frame work p50 16.3 / p90 17.5 ms, the governor steps to 40, and the fixed 1/60 step then plays as slow motion.
- The walked-distance check (this note's 3 s stick hold, 60 vs 30) has not been re-run on this build; the fast-forward it catches is exactly what the patch list removes.
- The `main` branch of nx2android (checked at `7cd66c3`) hangs at the initial load for this game — the main thread spins in an exclusive-access retry while every worker waits on `nn::os::WaitConditionVariable`, the picture stops and the audio goes on — where `v0.4.0` passes it; that is a host regression, apart from this profile, and is tracked in the host, not here.

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| AYN Thor (Snapdragon 8 Gen 2, Adreno 740, Android 13) | Adreno 740 | Turnip | 1.1.1 (v262144) | `v0.4.0` | playable |
| 2410DPN6CC (Snapdragon 8 Elite, Adreno 830, Android 16) | Adreno 830 | Qualcomm | 1.1.1 (v262144) | `v0.4.0` | 60 fps in the field, in-game menu, Chinese UI |

Tested on the AYN Thor (Adreno 740) with a Turnip Vulkan driver.

## Options

- Tested with the Turnip Vulkan driver: port with it (`--vulkan-driver`).

## Quirks and known issues

- Not recorded.
