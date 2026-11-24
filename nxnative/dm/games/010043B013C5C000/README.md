<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->
# NEO: The World Ends with You

![NEO: The World Ends with You](../../../../docs/images/neo-the-world-ends-with-you.jpg)

| | |
|---|---|
| Status | **playable** |
| Title ID | `010043B013C5C000` |
| Tested with nx2android | `v0.1.2` |
| Tested build and device | v196608 Lenovo Legion Y700 TB322FC (Snapdragon 8 Elite, Adreno 830, Android 16) |
| App package | `org.nx2a.neo` |
| Needs | a GPU that reads BC-compressed textures (Adreno, Xclipse; not Mali or PowerVR) |

The profile sets these for you (`android.tsv`); a player changes the pause menu ones in the game, and the env rows in the data pack's `env.txt`:

| Row | What it does |
|---|---|
| `docked=0` | pause menu default on first launch: docked mode (1) or handheld mode (0) |
| `overlay=1` | pause menu default on first launch: on-screen gamepad shown (1) or hidden (0) |
| `rumble=1` | pause menu default on first launch: rumble on (1) or off (0) |

Read the profile's porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list](../../../../README.md#games).
<!-- end of the game facts -->

## Game version

- Update 1.03.
- The profile was checked with `v196608` (the `tested` row of android.tsv).

## Frame rate

- Not recorded.

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| Lenovo Legion Y700 TB322FC (Snapdragon 8 Elite, Adreno 830, Android 16) | Adreno 830 | not recorded | 1.03 (v196608) | `v0.1.2` | playable |

## Options

- Not recorded.

## Quirks and known issues

- Not recorded.
