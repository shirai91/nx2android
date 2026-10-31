# Improving the core

The core is everything every game shares. Know which layer a change belongs to (docs/ARCHITECTURE.md "Layers and their rules"):

| Layer | Path | Notes |
|---|---|---|
| Shared engine (upstream) | `nxnative/dm/host` (Horizon services, threads, memory, loader), `dm/gfx/nvn` (the NVN layer the game calls), `dm/gfx/tools` (shader split, readers such as `unityfs.py`), `dm/tools` (`patch_nso.py` the rewriter, `nx_extract`), `dm/third_party/ryujinx` (the shader translator's base) | Shared with iOS. Android differences behind `__ANDROID__` / `__APPLE__` / `DM_TARGET` (`dm_uc.h`, `DM_ASM_*`, `dm_tsd_*` show the pattern). A third-party fix gets an entry in that folder's README. |
| Android runtime | `nxnative/dm/android/` | Vulkan replay (`gfx_vk_core/exec/tex/shader/present.c`), front end (`front_android.c`: input, on-screen gamepad, pause menu, text entry), `overlay_layout.h` (shared geometry), `perf_android.c`, `haptics_android.c`, `compat/`, `build.py` (NDK build), `apk/mkapk.py`, `nxnative/dm/android/tools/shader_vk.py`, `nxnative/dm/android/tools/android_profile.py` |
| Port pipeline | `porter/` | Stages in `pipeline.py`; reuses upstream's `nxnative/scripts/build_ipa_from_nsp.py` through `upstream.py` (imported, never copied). No UI. |
| GUI | `gui/` | Only calls `python -m porter …` (`runner.py`); anything it does, the CLI must do too. |
| Bundle | `packaging/` | `nx2android.py` is both the app and the Python the tools run under; a new tool started as `[sys.executable, script.py, …]` works frozen, other forms (`-c`, flags) do not. |

## How to make a core change

1. **Reproduce with evidence**: the `[RED]` line, crash backtrace, a screen recording, a census over all shaders. Find the semantics in the SDK / NVN / Vulkan behaviour, not in the one game.
2. **Write the general rule**, with the evidence in a comment beside it ("Viractal, Unity on SDK 21, calls it over and over from its title screen on, where it was a red stub"). If you must add a switch for A/B testing, make it an env variable read at use (`DM_VK_*` pattern) so it can be flipped live through DM_CTL.
3. **Check it against every game it can reach**: port and run NEO and Viractal (both, when the change is in shared code); for shader-tool changes compare outputs byte for byte where nothing should change (the Ryujinx fix left NEO's 25,514 shaders byte-identical).
4. **Build checks**: the host builds for each profile (`python nxnative/dm/android/build.py --game <id> --ndk tools/android-ndk-r27c --out <scratch> --lib [--release]`), `python -m unittest discover tests`.
5. **Device check** via [device-loop.md](device-loop.md); a change in the front end or overlay is checked by touch, controller keys and the pause menu.

## Specific areas

- **Shader translator**: Android builds `dm/gfx/tools/dmsh_xlate` over `dm/third_party/ryujinx` from source with the player's .NET SDK (porter `translator()`, once per source key, in the user folder), so a translator fix reaches Android at the next port. The iOS chain's prebuilt `dmsh_xlate_bin` and `dmsh_chain.lock` are Mac-only: don't touch them from here.
- **Shader readers** (`shader_split.py`, `unityfs.py` …): a reader that finds more shaders changes the profiles' tables; regenerate them as in [new-game-profile.md](new-game-profile.md) "Shader tables after a reader or translator fix". Unity layouts without type trees are keyed by their type hash in `unityfs.py KNOWN_TREES`.
- **Rewriter** (`dm/tools/patch_nso.py`): Android output differs by `DM_TARGET=android` (thread slots through `tpidr_el0[2]`, `emit_tp`); the host refuses modules rewritten for the other target. Per-build cuts stay in profiles' `patches.py` / `dm/tools/sdk_patches.py`.
- **Vulkan backend**: facts learned the hard way (in memory `neo-vulkan-findings`): NVN upper-left origin = negative-height viewport; the image-view cache keys on attachment vs sampling usage; Adreno LRZ goes stale across command-buffer cuts and within one buffer, and leaks into passes on other depth buffers (`lrz_touch`, `pass_commit`); frames are submitted in parts (`DM_VK_CB_DRAWS`, `DM_VK_SUBMIT_SPLIT`). Run the Khronos validation layer before guessing (`tools/vvl`, `mkapk.py --extra-so …/libVkLayer_khronos_validation.so`, env `DM_VK_VALIDATE=1`).
- **Front end / overlay**: the gamepad layout is data (`dm/android/overlay_default.tsv` + profile `overlay` rows → `android_profile.h` by `build.py`); don't hand-edit the generated table. The pause menu and text entry share the panel machinery (`dm_menu_state` → `ov_panel` → `dm_overlay_redraw`); geometry used by both touch and drawing lives in `overlay_layout.h`. Key events skip the IME (no `AInputQueue_preDispatchEvent`): the app shows no soft keyboard.
- **Settings and profile keys**: a new Android profile key goes in `android_profile.py` (parse + header), docs/ARCHITECTURE.md's key table, `tests/arch_rules.py`'s `KEYS`, and a unit test.
- **Porter**: a slow stage must keep its output keyed by its inputs (`nsp.fingerprint` / `reused`) and report through `PROG` so `--progress json` and the GUI show it; new options go in `cli.py` and, if the GUI needs them, through the CLI.

Remember the release build (`build.py --release`, the default port) drops DM_CTL and debug info; test diagnostics with `--debug` builds.
