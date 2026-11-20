<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->
# Fire Emblem Engage

![Fire Emblem Engage](../../../../docs/images/fire-emblem-engage.jpg)

| | |
|---|---|
| Status | **playable** |
| Title ID | `0100A6301214E000` |
| Tested with nx2android | `v0.4.0` |
| Tested build and device | v327680 AYN Thor (Snapdragon 8 Gen 2, Adreno 740, Android 13) |
| App package | `org.nx2a.t0100a6301214e000` |

Read the profile's porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list](../../../../README.md#games).
<!-- end of the game facts -->

## Game version

- Update 2.0.0.
- The profile was checked with `v327680` (the `tested` row of android.tsv).

## Frame rate

- 60 fps (30 on Switch).

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| AYN Thor (Snapdragon 8 Gen 2, Adreno 740, Android 13) | Adreno 740 | Turnip | 2.0.0 (v327680) | `v0.4.0` | playable |

Tested on the AYN Thor (Adreno 740) with a Turnip Vulkan driver; Qualcomm's own driver mis-draws Engage Attacks (#122).

## Options

- Tested with the Turnip Vulkan driver: port with it (`--vulkan-driver`).

## Quirks and known issues

- Not recorded.
