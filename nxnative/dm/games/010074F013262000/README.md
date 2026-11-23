<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->
# Xenoblade Chronicles 3

![Xenoblade Chronicles 3](../../../../docs/images/xenoblade-chronicles-3.jpg)

| | |
|---|---|
| Status | **wip** |
| Title ID | `010074F013262000` |
| Tested with nx2android | `v0.1.0` |
| Tested build and device | v655360 Lenovo Legion Y700 TB322FC (Snapdragon 8 Elite, Adreno 830, Android 16) |
| App package | `org.nx2a.xc3` |

The profile sets these for you (`android.tsv`); a player changes the pause menu ones in the game, and the env rows in the data pack's `env.txt`:

| Row | What it does |
|---|---|
| `DM_NVN_ASYNC=1` | asynchronous replay: submissions and presents run on a replay thread, syncs wait for the job's replay and then its GPU fence |

Read the profile's porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list](../../../../README.md#games).
<!-- end of the game facts -->

## Game version

- Update 2.2.0.
- The profile was checked with `v655360` (the `tested` row of android.tsv).

## Frame rate

- 30 fps.

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| Lenovo Legion Y700 TB322FC (Snapdragon 8 Elite, Adreno 830, Android 16) | Adreno 830 | not recorded | 2.2.0 (v655360) | `v0.1.0` | wip |

## Options

- Not recorded.

## Quirks and known issues

- **work in progress**: some effects and models don't draw yet.
