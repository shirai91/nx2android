<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->
# Portal 2

| | |
|---|---|
| Status | **playable** |
| Title ID | `0100ABD01785C000` |
| Tested build and device | 1.0.3 Lenovo Legion Y700 TB323FU (Snapdragon 8 Elite Gen 5, Adreno 840, Android 16) |
| App package | `org.nx2a.t0100abd01785c000` |

Read the profile's porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list](../../../../README.md#games).
<!-- end of the game facts -->

## Game version

- Base game 1.0.3 (the Switch build).
- Not played to the end: the title, the menus, a new save and chapter 1 were exercised on Android.

## Frame rate

- 60 fps steady in the title, the menus and chapter 1 (the adaptive frame-rate counter reports 60; footprint about 520 MiB).
- Later chapters are not measured.

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| Lenovo Legion Y700 TB323FU (Snapdragon 8 Elite Gen 5, Adreno 840, Android 16) | Adreno 840 | system | 1.0.3 | `main` with this PR's four fixes (no release contains them yet) | wip: title, menus, new save and chapter 1; 60 fps; audio works |

## Options

- None: the default options work.

## Quirks and known issues

- **It plays, but it is not perfect.** The title, the menus, a new save and chapter 1 were played; later chapters, long sessions and save re-imports beyond the first are untested. Some scenes can miss a few draws until the next capture round (see below), and the game was not played to the end.
- **A Switch save can hang the main menu on LOADING.** If the save's `config.cfg` holds `mat_monitorgamma_nx_docked` (or `..._nx_handheld`), the frame loop keeps presenting but the main thread waits forever inside the material system's queue and the menu never appears. Deleting those two lines from `config.cfg` lets it enter (bisected: `config.cfg` alone reproduces it; `nx_docked` alone reproduces it; `nx_handheld` alone does not). The host does not neutralise these convars yet, and a new save written by the port itself may set `nx_docked` again.
- **Shaders are compiled at runtime.** The game files contain no shaders; `subsdk0` is GLSLC. A shader the bundle does not hold is captured under `captured_shaders/` on the device and must be translated back into `shaders.dmsv` before it draws (see Porting notes). A first visit to a new area can miss a few draws; another capture round fixes them.
- Audio and input work: audio renders normally, and the microphone notification is bound but never signalled (the host has no microphone), which the game tolerates.

## Porting notes

- Engine: Source (NVIDIA Lightspeed). Modules `main subsdk0 sdk`; `subsdk0` exports `glslc*` and must be loaded, or materialsystem's compiler calls stay unimplemented and every draw is skipped (a black screen at 60 fps).
- The profile depends on four host/porter fixes shipped with it:
  - an `AcquireAudioDeviceNotificationForInput` HLE (SDK 13's form would IPC `audren:u`, which the host does not serve: `0xe15` Break at boot);
  - the shader-source classifier treating a game whose loaded modules export `glslc*` as NVN, not GL (Portal 2 imports EGL and `glShaderSource` names from SDL leftovers, which misclassified it as GL);
  - a capture-only shader bundle being allowed (the game has zero file shaders);
  - the Android capture loop serving `glslc_capture` instead of demanding a Mac host.
- Capture/re-inject loop used: play on the device, then `porter port --experimental --install` pulls `captured_shaders/` and translates them (`shader_vk.py --capture`) into `shaders.dmsv`; force-stop and relaunch. Observed convergence: 10 -> 51 -> 96 -> 99 -> 133, and after a from-scratch rebuild 10 -> 64 -> 138, with zero skipped draws and presents 599/600.
- Memory: `mem_reserve 0x100000000` (4 GB), `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`; the game sets a heap of the pool minus 64 MB. Footprint about 520 MiB in the tested route.
- Debugging the LOADING hang: a SIGALRM watch dump (main-thread backtrace, all guest threads) showed the main thread in the material system's MatQueue loop waiting on a sync event that never arrives while present keeps running; it is not a shader problem (zero captures and `skipped=0` during the hang). The trigger was isolated to the two `mat_monitorgamma_nx_*` rows as described above.
