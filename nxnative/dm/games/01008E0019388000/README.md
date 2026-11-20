<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->
# Ultra Kaiju Monster Rancher

![Ultra Kaiju Monster Rancher](../../../../docs/images/ultra-kaiju-monster-rancher.jpg)

| | |
|---|---|
| Status | **playable** |
| Title ID | `01008E0019388000` |
| Tested with nx2android | `v0.4.0` |
| Tested build and device | v0 AYN Odin 2 (Snapdragon 8 Gen 2, Adreno 740, Turnip driver), tutorials and battles |
| App package | `org.nx2a.kaijumonsterrancher` |

Read the profile's porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list](../../../../README.md#games).
<!-- end of the game facts -->

## Game version

- Update 1.0.1.
- The profile was checked with `v0` (the `tested` row of android.tsv).

## Frame rate

- 30 fps (60 runs the game twice as fast).

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| AYN Odin 2 (Snapdragon 8 Gen 2, Adreno 740, Turnip driver), tutorials and battles | Adreno 740 | Turnip | 1.0.1 (v0) | `v0.4.0` | playable |

Tested on the AYN Odin 2 (Adreno 740) with a Turnip Vulkan driver.

What was tested:

- Tested through the tutorials and battles.

## Options

- Tested with the Turnip Vulkan driver: port with it (`--vulkan-driver`).
- Handheld 3D resolution by default, docked in the app menu.

## Quirks and known issues

- Not recorded.
