<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->
# Kirby and the Forgotten Land

![Kirby and the Forgotten Land](../../../../docs/images/kirby-and-the-forgotten-land.jpg)

| | |
|---|---|
| Status | **playable** |
| Title ID | `01004D300C5AE000` |
| Tested with nx2android | `v0.4.0` |
| Tested build and device | v65536 AYN Thor (Snapdragon 8 Gen 2, Adreno 740, Android 13) |
| App package | `org.nx2a.kirbyandtheforgottenland` |
| Needs | a GPU that reads BC-compressed textures (Adreno, Xclipse; not Mali or PowerVR) |

Read the profile's porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list](../../../../README.md#games).
<!-- end of the game facts -->

## Game version

- Update 1.1.0.
- The profile was checked with `v65536` (the `tested` row of android.tsv).

## Frame rate

- 30 fps (as on Switch).

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| AYN Thor (Snapdragon 8 Gen 2, Adreno 740, Android 13) | Adreno 740 | Turnip | 1.1.0 (v65536) | `v0.4.0` | playable |

Tested on the AYN Thor (Adreno 740) with a Turnip Vulkan driver.

## Options

- Tested with the Turnip Vulkan driver: port with it (`--vulkan-driver`).

## Quirks and known issues

- Not recorded.
