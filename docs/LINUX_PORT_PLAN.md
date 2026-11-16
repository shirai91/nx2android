# Linux (aarch64) port: plan

Status (2026-11-16, branch `linux/burnout`, local only): phase 1 is done for Burnout Paradise Remastered. `dm/linux/build.py` builds `dmhost` for aarch64 glibc Linux (system clang, `--gfx null`); with the existing Android port's data pack (hard-linked into `.gamedump/BurnoutLinux/`) the game loads its three modules, mounts the RomFS, reaches `nnMain` and runs 600+ frames at a steady 60 Hz headless (one 40 s run; no window, no audio, no input). The Vulkan backend compiles except `gfx_vk_present.c` (JNI, `ANativeWindow`); nothing has been rendered on Linux yet.

Findings so far:
- glibc's thread pointer is the 2-word TCB followed by the executable's TLS, so `tpidr_el0[2]` (Bionic's `TLS_SLOT_APP`, read by the rewritten guest code) is the first word of the executable's TLS. `linux/compat/tls_slot.c` puts a `.tdata` variable there and `loader.c` checks the address at start-up. The Android-rewritten NSOs work unchanged.
- The host needed one change outside `dm/linux`: `crash.c` used `__ANDROID__` for a `/proc` thread-name read.
- The Vulkan backend's zero-copy pool design (an `AHardwareBuffer` mapped over the guest's pages) has a copying fallback; `linux/compat/include/android/hardware_buffer.h` stubs it to fail into that. The Linux equivalent to build later is a dma-buf exported by Mesa and mapped over the pages.
- The repo carries only `vk_video` headers; Vulkan headers and the loader come from the system (`--vulkan-inc`, `--vulkan-lib`).

Original plan, from a read-through of the tree: Target: the Retroid Pocket Nova running nova-nix (aarch64 glibc Linux, sway kiosk, Adreno 740 with Mesa Turnip), launched from its menu like the native ports there (`~/Games/<app>/game.json`, `runtime: native`).

## Why it looks feasible

The guest's arm64 code already runs in-process on Android, and the core (`dm/host`, `dm/gfx`) is shared with macOS and iOS. The Android runtime's own Android-only surface is small (files that mention each API, counted with grep):

| Android API | Files | Used for |
|---|---|---|
| `ANativeWindow` | 3 (`gfx_vk_present.c`, `front_android.c`, ...) | the Vulkan swapchain's surface and size |
| `AHardwareBuffer` | 5 | zero-copy GPU buffers |
| JNI (`JNIEnv`) | 4 | the Java front end: pause menu, gamepad overlay, text entry, saves |
| AAudio / MediaCodec | `compat/audiotoolbox_aaudio.c`, `audioconverter_mediacodec.c` | audio output and AAC |
| adrenotools | 8 | loading a custom Vulkan driver (Turnip) inside an app; not needed on Linux, where Mesa is the system driver |
| `__android_log`, `ALooper`, `AInputEvent` | 1-2 each | log, input loop |

The shared contract for a front end is `dm/host/front.h` (`front_run`, window/surface, `front_text_input`, `dm_touch_set`, `front_save_failed`). `front_mac.m` (297 lines) implements it for a desktop window; `front_android.c` (1197 lines) for NativeActivity.

## Proposed shape

- New `dm/linux/` runtime beside `dm/android/`: reuse the Vulkan backend (`gfx_vk_*`), replace the window with SDL2 (Wayland or X11, Vulkan surface through `SDL_Vulkan_CreateSurface`), input through SDL's gamepad API, audio through SDL or PipeWire in place of the AAudio shim, AAC through FFmpeg or a small decoder in place of MediaCodec.
- No pause menu in the game process. Nova's Home daemon already pauses the process group and draws the pause panel, so the Java overlay/pause menu is not ported. (To check: how the host reacts to SIGSTOP, and whether audio and timers survive it.)
- `build.py` gets a Linux target: a native aarch64 compiler instead of the NDK, glibc instead of Bionic. Expect small libc differences in the `compat/` layer and in `entry.S` assumptions.
- `porter` gets a `linux` output next to the APK: the host binary, `shaders.dmsv`, RomFS and the rest of the data pack in one folder, plus a `game.json` for nova-launch. Nothing from a game goes in git.

## Phases

1. Build `libdmhost` for aarch64 Linux with a null window (`gfx_null.c` exists) and boot a small game to its first frames headless. This shows how far glibc is from Bionic.
2. SDL2 front end: window, Vulkan present, gamepad, then audio. First target: a game already marked `playable` at 60 fps on an Adreno 740 with Turnip (README: Super Mario 3D World, Pokémon Scarlet, Kirby on the AYN Thor).
3. Porter output and a Nova launch entry; screenshot and play-test on the device.
4. Saves and text entry through the Nova's paths and the menu's keyboard.

## Open questions

- Does Mesa Turnip on the Nova expose what `gfx_vk_*` needs (BC texture compression, the features listed per profile in `android.tsv` `requires`)? Check with `vulkaninfo` on the device first.
- Memory: the guest needs a large address-space reservation (`--alias-headroom`); check overcommit limits on the Nova's kernel.
- The Nova has 12 GB or less of RAM shared with the GPU; heavy games (Scarlet, TotK) may not fit.
- Licence and distribution: the same rule as the Android port, bring your own files and keys; the Linux output stays out of git.
