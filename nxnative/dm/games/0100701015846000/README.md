<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->
# Unpacking

![Unpacking](../../../../docs/images/unpacking.jpg)

| | |
|---|---|
| Status | **playable** |
| Title ID | `0100701015846000` |
| Tested with nx2android | `v0.1.2` |
| Tested build and device | v327680 Lenovo Legion Y700 TB322FC (Snapdragon 8 Elite, Adreno 830, Android 16) |
| App package | `org.nx2a.unpacking` |

The profile sets these for you (`android.tsv`); a player changes the pause menu ones in the game, and the env rows in the data pack's `env.txt`:

| Row | What it does |
|---|---|
| `overlay=0` | pause menu default on first launch: on-screen gamepad shown (1) or hidden (0) |
| `DM_TOUCH_DOCKED=screen` | touch in docked mode: "screen" = the screen's points, "pixels" = the game's pixels; unset = no touch when docked |

Read the profile's porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list](../../../../README.md#games).
<!-- end of the game facts -->

## Game version

- Update 2.01.
- The profile was checked with `v327680` (the `tested` row of android.tsv).

## Frame rate

- 60 fps.

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| Lenovo Legion Y700 TB322FC (Snapdragon 8 Elite, Adreno 830, Android 16) | Adreno 830 | not recorded | 2.01 (v327680) | `v0.1.2` | playable |

## Options

- Played by touch.

## Quirks and known issues

- Not recorded.
