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

The profile sets these for you (`android.tsv`); a player changes the pause menu ones in the game, and the env rows in the data pack's `env.txt`:

| Row | What it does |
|---|---|
| `DM_AUDREN_CATCHUP_FRAMES=200` | catch-up frames (a frame rendered on the updating thread when nothing was rendered since the last update) only while fewer than this many frames have been … |

Read the profile's porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list](../../../../README.md#games).
<!-- end of the game facts -->

## Game version

- Update 1.2.0 (`v327680`), the version the profile's 60 fps patches are made for. Port the base game together with this update.
- The DLC (Cindered Shadows and the other packs) were not ported or tested.

## Frame rate

- 60 fps (30 on Switch) on the title, in the dialogue and the menus, on the first battle map and in its 3D combat scenes, and in the monastery, docked and handheld. On a Legion Y700 (2025), handheld: the battle map and the combat scenes take about 7-9 ms of GPU time a frame, the monastery 4-5 ms, with no frames dropped on screen.
- The anime movies play at their own 30 fps.

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| Lenovo Legion Y700 (2025, TB322FC) | Adreno 830 | system | 1.2.0 (v327680) | `v0.5.0` | playable: title, movies, class demo, new game, character creation, prologue, first battle won with its combat scenes, Chapter 1 story and monastery free roam, save and load, pause menu, 30-minute battle soak; sound checked with a recording of the output (no repeats, gaps or clipping in 20 minutes) |

## Options

- None: the default options work. Use the device's own Vulkan driver: Turnip draws the game too but takes about three times the GPU time.
- The port boots the game once on the device to capture the 9 shaders the game compiles when it starts, so connect the device (USB debugging on) even when you port without installing.

## Quirks and known issues

- None known.
- Name entry opens the on-screen keyboard; press + (OK) to confirm the name, as the game's help line says.
