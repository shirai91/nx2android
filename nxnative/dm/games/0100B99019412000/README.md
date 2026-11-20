<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->
# Mario vs. Donkey Kong

![Mario vs. Donkey Kong](../../../../docs/images/mario-vs-donkey-kong.jpg)

| | |
|---|---|
| Status | **playable** |
| Title ID | `0100B99019412000` |
| Tested with nx2android | `v0.1.3` |
| Tested build and device | v65536 Lenovo Legion Y700 TB322FC (Snapdragon 8 Elite, Adreno 830, Android 16) |
| App package | `org.nx2a.mariovsdonkeykong` |

The profile sets these for you (`android.tsv`); a player changes the pause menu ones in the game, and the env rows in the data pack's `env.txt`:

| Row | What it does |
|---|---|
| `DM_MOVIE_AUDIO_SLOTS=32` | audio output buffers of a movie with sound (4 up to the decoder's maximum) |

Read the profile's porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list](../../../../README.md#games).
<!-- end of the game facts -->

## Game version

- Update 1.0.1.
- The profile was checked with `v65536` (the `tested` row of android.tsv).

## Frame rate

- 60 fps.

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| Lenovo Legion Y700 TB322FC (Snapdragon 8 Elite, Adreno 830, Android 16) | Adreno 830 | not recorded | 1.0.1 (v65536) | `v0.1.3` | playable |

## Options

- Not recorded.

## Quirks and known issues

- Not recorded.
