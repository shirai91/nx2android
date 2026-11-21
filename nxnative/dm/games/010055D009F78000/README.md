<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->
# Fire Emblem: Three Houses

![Fire Emblem: Three Houses](../../../../docs/images/fire-emblem-three-houses.jpg)

| | |
|---|---|
| Status | **playable** |
| Title ID | `010055D009F78000` |
| Tested with nx2android | `v0.5.0` |
| Tested build and device | v327680 Lenovo Legion Y700 TB322FC (Snapdragon 8 Elite, Adreno 830, Android 16) |
| App package | `org.nx2a.fireemblemthreehouses` |

Read the profile's porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list](../../../../README.md#games).
<!-- end of the game facts -->

## Game version

- Update 1.2.0 (`v327680`), the version the profile's 60 fps patches are made for. Port the base game together with this update.
- The DLC (Cindered Shadows and the other packs) were not ported or tested.

## Frame rate

- 60 fps (30 on Switch), in the monastery scenes, the dialogue, the menus and on the battle map. The battle map takes about 10-12 ms of GPU time a frame on a Legion Y700 (2025).
- The anime movies play at their own 30 fps.

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| Lenovo Legion Y700 (2025, TB322FC) | Adreno 830 | system | 1.2.0 (v327680) | `v0.5.0` | playable: title, movies, class demo, new game, character creation, prologue, first battle, save and load, 30-minute battle soak |

## Options

- None: the default options work. Use the device's own Vulkan driver: Turnip draws the game too but takes about three times the GPU time.
- The port boots the game once on the device to capture the 9 shaders the game compiles when it starts, so connect the device (USB debugging on) even when you port without installing.

## Quirks and known issues

- None known.
- Name entry opens the on-screen keyboard; press + (OK) to confirm the name, as the game's help line says.
