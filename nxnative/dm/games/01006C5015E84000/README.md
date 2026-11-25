<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->
# Disco Elysium - The Final Cut

![Disco Elysium - The Final Cut](../../../../docs/images/disco-elysium-the-final-cut.jpg)

| | |
|---|---|
| Status | **playable** |
| Title ID | `01006C5015E84000` |
| Tested with nx2android | `v0.4.0` |
| Tested build and device | v0 (1.0.7) Sony XQ-DQ72 (Adreno 740) and OnePlus 15 (Snapdragon 8 Elite Gen 5), 2026-11-10 |
| App package | `org.nx2a.t01006c5015e84000` |

The profile sets these for you (`android.tsv`); a player changes the pause menu ones in the game, and the env rows in the data pack's `env.txt`:

| Row | What it does |
|---|---|
| `docked=0` | pause menu default on first launch: docked mode (1) or handheld mode (0) |

Read the profile's porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list](../../../../README.md#games).
<!-- end of the game facts -->

## Game version

- Base game 1.0.7.
- The profile was checked with `v0` (the `tested` row of android.tsv).

## Frame rate

- 30 fps (as on Switch).

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| (1.0.7) Sony XQ-DQ72 (Adreno 740) and OnePlus 15 (Snapdragon 8 Elite Gen 5), 2026-11-10 | Adreno 740 | not recorded | 1.0.7 (v0) | `v0.4.0` | playable |
| POCO X3 Pro (Snapdragon 860), community test, 2026-11-24 | Adreno 640 | Turnip, Mesa 27.0.0-devel (git-c5ef4bdfab) | 1.0.13 (as reported) | `v0.5.0` | renders correctly; one 37-minute session |

Tested on the Sony XQ-DQ72 and the OnePlus 15. A community tester confirmed the Adreno 640 rendering fix on the POCO X3 Pro.

## Options

- Not recorded.

## Quirks and known issues

- Block and triangle artifacts on Adreno 640 with Turnip
  ([issue #283](https://github.com/shirai91/nx2android/issues/283)) came from 219
  fragment shaders that form coarse derivatives with indexed quad shuffles. That
  driver offers subgroup operations only in compute (`stages 0x20`), so these
  shaders ran on the one-invocation fallback. The translator now emits native
  coarse derivatives for this form. The POCO X3 Pro run above logs no fallback
  shaders and renders the game correctly.
