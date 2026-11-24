<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->
# DRAGON QUEST VII Reimagined

![DRAGON QUEST VII Reimagined](../../../../docs/images/dragon-quest-vii-reimagined.jpg)

| | |
|---|---|
| Status | **experimental** |
| Title ID | `0100505021440000` |
| Tested with nx2android | `v0.5.0` |
| Tested build and device | v65536 Lenovo Legion Y700 TB321FU (Snapdragon 8 Gen 3, Adreno 750, Android 16) |
| App package | `org.nx2a.dragonquestvii.asia` |
| Vulkan driver | Turnip: the port packs Mesa's Turnip driver when the porter finds it (`vulkan_driver turnip`) |

The profile sets these for you (`android.tsv`); a player changes the pause menu ones in the game, and the env rows in the data pack's `env.txt`:

| Row | What it does |
|---|---|
| `DM_GPU_TURBO=auto` | "auto" or 1: ask the GPU for its high clocks while the game is GPU-bound; "need": only while the game is held below its target; unset or 0 = no |

Read the profile's porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list](../../../../README.md#games).
<!-- end of the game facts -->

## Game version

- Update 1.1.0 (v65536), the Asian (Korean / Chinese) release. The JP/WW release has its own profile for the Mac (`0100A9D01C446000`), which the shader split, the allocator cut and the memory layout here follow: its SDK and movie module builds are the same.
- Other updates are not tested.

## Frame rate

- Up to 60 fps, with the frame-rate governor stepping between 60, 40 and 30. On the tested device handheld (1280x720) reaches 60 fps on the overworld and 40 in the village; docked (1920x1080) holds 30, at a higher SoC temperature. The game itself caps Unreal at 30 and asks for present interval 2; the profile lifts both (`fps_locks.tsv`). The operation mode is the pause menu's own setting, and it decides the render resolution.
- The GPU is asked for its high clocks while it is the limit (`DM_GPU_TURBO=auto`). Without that, a held frame rate lets the GPU governor lower the clock until the work fills the longer frame: measured at ~22 fps with a cool SoC, against 30 fps held once the clocks were asked for. A low temperature alongside a low frame rate is that downclocking, not headroom.

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| Lenovo Legion Y700 TB321FU (Snapdragon 8 Gen 3, Android 16) | Adreno 750 | Turnip 26.2.3 | 1.1.0 (v65536) | `v0.5.0` | experimental: the title, a new game, the opening, the first village and its surroundings (walking, dialogue, battles), village transitions, the pause menu, an autosave and Continue from it |

## Options

- `--vulkan-driver <vulkan.turnip.so>`: the port needs Mesa's Turnip (26.2.3 tested). The Adreno 750's system driver (`libllvm-qgl.so`) crashes at boot compiling the first compute pipeline; the porter packs Turnip into the APK when it finds it.
- `--language zh-Hans` (or `zh-Hant`, `ko`): the game language; this is the Korean / Chinese release.
- `--app-name`: the launcher label. The control data names the game in Korean; pass the name you want (`DRAGON QUEST VII Reimagined`).

## Quirks and known issues

- None known.