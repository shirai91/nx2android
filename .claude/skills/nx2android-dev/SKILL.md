---
name: nx2android-dev
description: How to add a new game profile to nx2android (port another Switch game to Android) or improve the shared core (the Android runtime, the Vulkan backend, the Horizon host, the rewriter, the shader tools, the porter or the GUI). Use whenever the task is to port or bring up a game on Android, add or edit a profile under nxnative/dm/games/ or an android.tsv, decide whether a fix belongs in a profile or the core, make a core change (missing SDK/NVN functions, shader readers or the translator, input, the overlay, the pause menu), install a build on the device, or change porter/, gui/, packaging/ or nxnative/dm/android/ — even if the user only says "make <game> work" or "port <game>". Chasing a symptom on the device (flicker, crashes in the app, refused submissions, stutter) or measuring speed and stability is the switch-android-port skill's; it sends the fix back here.
---

# nx2android: game profiles and the core

nx2android turns a player's own Switch game files into an Android app: `porter` runs the port stages, `gui` draws them, `nxnative/` (our own fork of seedds/nxnative since 2026-10-29; no `upstream` remote, never pulled from) holds the engine. Every supported game is a **profile**; everything shared is the **core**. The contract is [docs/ARCHITECTURE.md](../../../docs/ARCHITECTURE.md): read it before changing anything, and keep it true.

## First decide: profile or core?

| The problem | Where the change goes |
|---|---|
| A game-specific constant, setting default, package name, device need, gamepad position, env switch | The profile: `nxnative/dm/games/<title ID>/android.tsv` (or upstream's `game.tsv`) |
| A fix in the game's own binary for one build (a cut, a gate) | The profile's `patches.py`, keyed by build ID (upstream's mechanism) |
| Anything that is true of Horizon, NVN, Vulkan, an engine version or the device | The core, as a **general rule** that holds for every game |

A fix that only one game needs today is still a core fix when it follows the semantics (Viractal brought `nvnTextureViewCompare`, NVN advanced blend modes, Unity 6.3's ComputeShader layout and a shader-translator fix: all general). A per-game branch in code is never the answer: a game that needs different behaviour gets a profile key. Comments may name the game a rule was found with.

- New game → [references/new-game-profile.md](references/new-game-profile.md)
- Core change → [references/core-changes.md](references/core-changes.md)
- Either way, testing on the device → [references/device-loop.md](references/device-loop.md)
- Chasing a symptom on the device (flicker, crashes in the app, refused submissions, stutter), measuring speed or stability → the **switch-android-port** skill ([.claude/skills/switch-android-port](../switch-android-port/SKILL.md)): playbooks, every debug switch, the NEO case study and device scripts.

## Rules that always hold

- **Ask before installing on the user's device.** Name the package and size; install test builds under another package (`--package org.nx2a.<name>.test`) beside the user's own app, and uninstall them afterwards. Someone at the tablet once removed an unannounced test app by hand. Never uninstall or overwrite the user's own install (`org.nx2a.neo` etc.) without being told to.
- **Keys and game files never enter the repository or the chat.** Print key names, never values.
- **Game files go only in `.gamedump/<App>/`** (the main checkout's, git-ignored), as hard links to the downloads, for every port, test or check: never a folder elsewhere on the PC (`C:\nx2a-test` once kept 263 GB that nobody deleted). Delete the folder when the work is done: references/new-game-profile.md §0 and §5.
- **Measure, then change.** Every fix starts from evidence (a log line, a crash address, a census, a screen recording), and the evidence goes in the comment beside the fix, as the existing code does.
- **English everywhere; log lines for something unimplemented or wrong start with `[RED] `.**
- **Keep the shared core shared.** Changes in `nxnative/dm/{host,gfx,tools,third_party}` keep the iOS paths working and platform differences go behind `__APPLE__` / `__ANDROID__` / `DM_TARGET`. Android-only code belongs in `nxnative/dm/android/`.
- **Match the surrounding code**: its comment density, naming and dense one-line style.
- **Tests before committing**: `python -m unittest discover tests` (includes `tests/arch_rules.py`, which enforces ARCHITECTURE.md's layer rules: no game IDs, names or packages in core code, data-only profiles, GUI only over `python -m porter`…). Commit and push only when the user asks; end commit messages with the attribution line the session gives. Messages are Conventional Commits (`fix(vk): …`, `feat(profile): …`, `docs(status): …`; docs/DEVELOPMENT_GUIDE.md's Commit messages), checked by `.githooks/commit-msg`.
- **Update the docs with the code**: ARCHITECTURE.md when a layer, key or stage changes; README.md's Games table when a status changes (the only place it is kept: docs/PORT_STATUS.md only points there); the profile's `android.tsv` comment header with what was tested.
- **Every finished port updates both game lists, in the same commit as the profile**: docs/PORT_CHECKLIST.md (tick the game, adding its line in alphabetical order if it is missing, drop "profile here" / "upstream profile", recount the `N / M ported` line) and README.md's Games table (its row, with a real device screenshot in docs/images/, never an icon or internet image, and the nx2android version tag: references/new-game-profile.md §4). SMO and Metroid Dread once shipped in the games table only.
- **Releases do not re-test older games** (since 2026-10-31): a release checks only the games it adds or fixes; a break in an older game is fixed by a later patch release. Version `0.MINOR.PATCH`: patch for new games and bug fixes only, minor for any other core change, major stays 0 (docs/DEVELOPMENT_GUIDE.md, Releases and version numbers).

## Windows environment gotchas (this PC)

- Git Bash mangles `/sdcard/...` arguments: `export MSYS_NO_PATHCONV=1` before adb or scripts that take device paths.
- Heredocs strip backslashes in Python one-offs: write a scratch `.py` file or use the Edit tool instead.
- The engine's tools need `PYTHONUTF8=1` (porter sets it); `python` with `PYTHONUTF8=1` also avoids cp1252 errors when printing.
- Toolchains for development are in the repo's untracked `tools/` (NDK r27c, .NET SDK 10, platform-tools, the Vulkan validation layer in `tools/vvl`); the player's settings are in `%APPDATA%\nx2android\config.json` (`NX2A_HOME` overrides the folder, which also holds the APK signing key and the built shader translator).
- adb: `tools/platform-tools/adb.exe`; the device is in `devtools/device.json` (serial and package). Big pushes go through `devtools/push_chunked.py` / porter (adb compression off, `-Z`), or adbd wedges.
