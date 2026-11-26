<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->
# Pokémon Sword

![Pokémon Sword](../../../../docs/images/pokemon-sword.jpg)

| | |
|---|---|
| Status | **playable** |
| Title ID | `0100ABF008968000` |
| Tested with nx2android | `v0.4.0` |
| Tested build and device | v458752 AYN Odin 2 (Snapdragon 8 Gen 2, Adreno 740, Android 13) |
| App package | `org.nx2a.pokemonsword` |

The profile sets these for you (`android.tsv`); a player changes the pause menu ones in the game, and the env rows in the data pack's `env.txt`:

| Row | What it does |
|---|---|
| `DM_VK_CUBE_WIDEN=1` | 1 = a cube read through fewer than six faces of a cube-compatible image takes the whole cube (Pokemon Sword's reflection probe; otherwise refused and read as … |

Read the profile's porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list](../../../../README.md#games).
<!-- end of the game facts -->

## Game version

- Update 1.3.2.
- The profile was checked with `v458752` (the `tested` row of android.tsv).

## Frame rate

- 30 fps (as on Switch; 60 runs the game twice as fast).

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| AYN Odin 2 (Snapdragon 8 Gen 2, Adreno 740, Android 13) | Adreno 740 | Turnip | 1.3.2 (v458752) | `v0.4.0` | playable |

Tested on the AYN Odin 2 (Adreno 740) with a Turnip Vulkan driver.

What was tested:

- **tested to the Wild Area**, wild battles included.

## Options

- Tested with the Turnip Vulkan driver: port with it (`--vulkan-driver`).

## Quirks and known issues

- Handheld mode draws blurred (docked is the default).
