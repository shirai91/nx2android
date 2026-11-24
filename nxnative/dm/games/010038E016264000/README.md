<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->
# Mega Man Battle Network Legacy Collection Vol. 1

![Mega Man Battle Network Legacy Collection Vol. 1](../../../../docs/images/mega-man-battle-network-legacy-collection-vol-1.jpg)

| | |
|---|---|
| Status | **playable** |
| Title ID | `010038E016264000` |
| Tested with nx2android | `v0.4.0` |
| Tested build and device | v131072 (1.0.2) AYN Odin 2 (QCS8550, Adreno 740, Android 13, Turnip), 2026-11-14 |
| App package | `org.nx2a.mmbnlc1` |
| Vulkan driver | Turnip: the port packs Mesa's Turnip driver when the porter finds it (`vulkan_driver turnip`) |

The profile sets these for you (`android.tsv`); a player changes the pause menu ones in the game, and the env rows in the data pack's `env.txt`:

| Row | What it does |
|---|---|
| `pacing steady` | frame pacing preset: DM_ADAPTIVE_FPS=0 |
| `DM_MOVIE_FAST=1` | the movie frame's NEON NV12 -> RGBA conversion (bit-exact) and block-linear copies a whole GOB at a time (the same addresses) |

Read the profile's porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list](../../../../README.md#games).
<!-- end of the game facts -->

## Game version

- Update 1.0.2.
- The profile was checked with `v131072` (the `tested` row of android.tsv).

## Frame rate

- 60 fps, held.

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| (1.0.2) AYN Odin 2 (QCS8550, Adreno 740, Android 13, Turnip), 2026-11-14 | Adreno 740 | Turnip | 1.0.2 (v131072) | `v0.4.0` | playable |

Tested on the AYN Odin 2 (Adreno 740) with a Turnip Vulkan driver (Qualcomm's driver crashes or runs at 30).

What was tested:

- Battle Network 1, 2 and 3 played: saves, battles, maps, gallery.

## Options

- Tested with the Turnip Vulkan driver: port with it (`--vulkan-driver`).
- The launcher's menus draw fully from the second port with the device connected after a first run (it builds some shaders at run time).

## Quirks and known issues

- Not recorded.
