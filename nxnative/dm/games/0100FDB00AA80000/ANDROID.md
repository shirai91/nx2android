# Layton's Mystery Journey: Katrielle and the Millionaires' Conspiracy DX+ on Android

Status: **playable** — ported and played on an Android device.

The Japanese release of Layton's Mystery Journey DX+, the 3DS game remastered for the Switch. It covers **1.1.0 (`v65536`)**, title ID `0100FDB00AA80000`, and its DLC `0100FDB00AA8100A`. Other game versions are outside this assessment.

## Compatibility evidence

The game renders with **OpenGL through EGL**: `main` imports the GL entry points by name, and `subsdk0` is NintendoSDK_OpenGL, which the Android GL layer replaces (`gl_android.c`). `subsdk1` is NintendoSDK_movie, loaded for the AVC opening movie. There are no NVN shaders: the profile's NVN tables stay empty, and the game's GLSL compiles through the device's GL driver.

The GL layer covers what this title needs: by-name GL imports (`dm_gl_import`), unsized internal formats (`ifmt_ok`), and the legacy `GL_ALPHA` / `GL_LUMINANCE` / `GL_LUMINANCE_ALPHA` uploads converted to RGBA8 (the 4096x1024 glyph atlas and the movie's Y and UV planes). The time profile takes `AdjustableUserSystemClock::Adjust` over: the SDK's `time::Initialize` is a no-op, and `Adjust` would write a null service object. The rewriter's jump-table fix handles `main`'s `and x18, x10, #0xffffffff` index copy.

Relevant existing paths:
- `nxnative/dm/android/gl_android.c`
- `nxnative/dm/host/hle_time.c`
- `nxnative/dm/tools/patch_nso.py`

## Build and defaults

| Module | Build ID |
|---|---|
| `main` | `517fb1e9589bb7999baa8bc673c008f9` |
| `subsdk1` | `2e8d3a967b3539eb77f50d1647380378` |
| `sdk` | `f7c7b7a90ecdc2d41c97508cc7236b8a` |

Android starts docked, with the on-screen gamepad visible. The engine is fixed-step at 30 fps, so the frame rate stays 30 whatever the host presents.

## Device testing

Tested on the **iQOO 9 (V2171A)** (Snapdragon 8 Gen 1, Adreno 730, Android 15), the player's own dump and keys:

1. The port of the version above finishes: the extraction, the rewrite (226/226 pieces) and the data pack complete, with no `[RED]` line.
2. The title logo, the menus, the dialogue text and the opening AVC movie draw correctly; the detective-office dialogue, the field and the daily-puzzle list were played.
3. 29.6-30 fps, the engine's own step; 0 audio underruns in the 2,000 callbacks the log counts.
