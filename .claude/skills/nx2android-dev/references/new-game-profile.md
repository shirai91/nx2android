# Adding a game profile

A profile is data only: `nxnative/dm/games/<base game's title ID>/` with upstream's `game.tsv` and shader tables (`shader_map.tsv` + `.conflicts.tsv`, `shader_facts.tsv`, `shader_pairs.tsv`, `pso_manifest.tsv`, `bf_want.tsv`), an optional `patches.py`, and nx2android's `android.tsv`. Upstream's key reference is `nxnative/dm/games/README.md`; Android keys are in docs/ARCHITECTURE.md "Game profiles on Android". Viractal (`0100FD501F95A000`) was brought up this way and is the worked example.

## 0. Can it be ported at all?

Run `python -m porter inspect "<game folder>"`. The port refuses:
- a game whose `main` renders with Vulkan or OpenGL (the Android host replays NVN only);
- one whose shaders need a step upstream runs by booting the game on a Mac (GLSLC capture, Unreal / Oodle decoding).
The game folder for any port, test or check is `.gamedump/<App>/` in the main checkout (git-ignored; `<App>` is the APK's name, e.g. `HadesII`), also from a worktree, and never a folder elsewhere on the PC: hard-link the downloaded NSP / NSZ / XCI files into it (`ln`, no copy; porter decompresses NSZ and XCZ itself), and never write into the downloaded game's folder. The port's `_build`/`_survey` folders, APK and data pack then land there too, and the folder is deleted when the work is done (§5). If the game has an iOS profile in `dm/games/` (52 do), its constants are already proven: start from it.

## 1. Experimental port

```
python -m porter port "<game folder>" --experimental --debug -k <prod.keys>
```

- No profile: upstream's survey writes `<game folder>/<id>_survey/survey.md` (modules, build IDs, SDK, engine markers, RomFS containers, rewrite-site census, imports vs host hooks) and a provisional profile guessed from the files.
- iOS-only profile: it is copied.
- Either way the profile lands in `<game folder>/<id>_build/profile/<id>/` with an `android.tsv` of status `experimental`, found by every tool through `DM_GAMES_DIR`. Nothing in the repository changes yet.
- Read survey.md before going further, and the stage logs in `<id>_build/logs/` when a stage fails.

Then install it under a test package (ask the user first) and follow [device-loop.md](device-loop.md):

```
python -m porter port "<game folder>" --experimental --debug --install --package org.nx2a.<short>.test --device <serial>
```

## 2. Bring it up

Iterate: read `log.txt`, find the first `[RED]` line or crash, fix, port again (only changed stages rerun). Typical first failures and where they are fixed:

| Symptom | Where |
|---|---|
| A stub named in a `[RED]` line (an SDK / NVN function the host lacks) | Implement it in the core by its semantics (`dm/host/hle_*.c`, `dm/gfx/nvn/hle_nvn.c`) — see [core-changes.md](core-changes.md) |
| `program shader … is not in the bundle` / shaders failing to translate | The shader split's readers (`dm/gfx/tools/shader_split.py`, `unityfs.py` …) or the translator (`dm/gfx/tools/dmsh_xlate`, `dm/third_party/ryujinx`) |
| Wrong rendering | The Vulkan backend `dm/android/gfx_vk_*.c`; diagnostics in [device-loop.md](device-loop.md) |
| Crash in the game's own code on one build | Upstream's bring-up catalogue `nxnative/docs/agents/03-bringup.md`; a per-build cut in the profile's `patches.py` only when no general rule fits |
| Memory / address-space layout | `game.tsv` keys (upstream README), checked against the survey |

Upstream's agent guide (`nxnative/docs/agents/`: 01-extract, 02-survey, 03-bringup, 04-graphics, 05-debugging) is written for the Mac host but its failure catalogue and survey method apply; the Mac-only steps (lldb, the Mac host, Metal) do not run here.

Play past the title into real gameplay before calling it more than `boots`: Viractal's first two "soft-locks" were the game's own responses to wrong inputs, and its name entry needed a core feature (the overlay keyboard).

## 3. Move the profile into the repository

When the game boots on the device:

1. Copy the profile files (not the build folder's other contents) into `nxnative/dm/games/<id>/`: `game.tsv`, `shader_map.tsv`, `shader_map.tsv.conflicts.tsv`, `shader_facts.tsv`, `shader_pairs.tsv`, `pso_manifest.tsv`, `bf_want.tsv` (and `patches.py` if one was written).
2. Check `game.tsv` by hand against survey.md (the survey's values are guesses where the README says so); scrub any local path from its comments.
3. Write `android.tsv`: a comment header (game, engine, SDK, what was tested on which device and date, known gaps), then rows. Only these keys exist:
   ```
   status       playable | boots | experimental
   tested       <version> <device>
   package      org.nx2a.<name>            (default org.nx2a.t<title id>)
   orientation  landscape | any
   requires     bcn                        (BC textures; the installer checks)
   setting      overlay=1 / rumble=1 / docked=0   (pause-menu defaults → settings.txt)
   env          KEY=VALUE                  (a host switch the game needs → env.txt)
   overlay      <control> <x> <y> [<r>]    (moves one control of dm/android/overlay_default.tsv)
   ```
   `android.tsv` is not part of the shader keys: editing it re-translates nothing.
4. Port again without `--experimental`; it must reuse or rebuild cleanly from the repository profile.

**Shader tables after a reader or translator fix.** For a profile in `dm/games`, the split refuses when the tables it makes differ from the profile's ("the split shader tables differ from game profile …"), so a change that finds more shaders stops the next port. The new tables are left in `<id>_build/shader/pkg.partial/tables/`: diff them against the profile's (more rows, none lost, the shaders that were missing now present), copy the three tables in, port again. Check the same for every other profile the change could touch (port NEO too).

## 4. Playable

- Play it on the device: title, menus, gameplay, a save, the pause menu, the on-screen gamepad, a controller if the game uses one, text entry if it asks for names, a few minutes of steady frame rate (`[perf]` lines) and no new `[RED]` lines.
- Set `status playable` and `tested <version> <device>`; update the header comment and the Games table in README.md. Its picture is a gameplay frame captured on the device while porting, saved as a 1280x720 JPEG in docs/images/: a frame shot (`DM_VK_SHOTS=1`, the switch-android-port skill's `frame_shots.sh`) or the last pass of a pass dump, so the on-screen gamepad is not in it (a game whose window origin is bottom-up, like Celeste, comes out upside down: flip it). Never the game's icon or an image from the internet.
- The row's `<sub>` line starts with one tag, the nx2android version the game was tested on: `` `v0.1.3` · update 1.10 · 60 fps ``. A port on the development checkout takes the next release's version: the patch after the latest tag if `main` has only new games and bug fixes since it, else the next minor (docs/DEVELOPMENT_GUIDE.md, Releases and version numbers). This is not the profile's `tested <version> <device>`, which names the game's update.
- Tick the game in docs/PORT_CHECKLIST.md: ``- [x] <name> · `<title ID>` `` with the "profile here" / "upstream profile" note dropped (add the line in alphabetical order if the game is not listed), and recount the `N / M ported` line from the `- [x]` and `- [` lines. Both docs go in the profile's commit; a port is not done until both show it.
- Run `python -m unittest discover tests`; uninstall the test app; commit when asked.

## 5. Clean up

The PC has little disk space. Once the port is committed and the test app uninstalled, delete `.gamedump/<App>/`: first check that each dump file in it has a link count of 2 or more (`fsutil hardlink list <file>`), so the downloaded copy remains; a file with one link is the only copy, so stop and ask. Keep a folder that other work still uses, and say so. A test or check that is not a port ends the same way.
