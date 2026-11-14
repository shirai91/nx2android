---
name: switch-android-port
model: sonnet
description: Debug a Switch game running on an Android device under nx2android - crashes in the app, rendering bugs, stutter and frame rate, soak tests and play-session checks. Use when chasing a symptom on the device or measuring speed or stability there.
---

# Bringing a port up on the device

The device side of a port: make a problem countable, find its cause, prove the fix. Read `nx2android-dev` only to decide where a fix goes (profile or core) or for installing and launching (`nx2android-dev/references/device-loop.md`).

Read only when needed:
- `references/playbook.md`: the symptom table (what each rendering, audio or crash symptom turned out to be) and the full performance playbook (frame pacing, governor, threads).
- `references/switches.md`: every host and backend switch, and whether it changes live.
- `references/neo-case-study.md`: NEO's bring-up, bug by bug, with numbers and dead ends.

## Rules for working on the device

These came from mistakes on NEO. `nx2android-dev` has the general rules: ask before installing, keys stay out, commit only when asked.
- **The device is shared.** Check what is on screen before driving it. If the user is holding it or playing, ask before restarting or pressing anything.
- **Never press A (confirm) blindly in the user's save.** Blind presses advanced NEO's story. Boot with `scripts/boot_to_gameplay.sh` (it presses only while a detector says "not gameplay yet"), and keep soak tests to the stick and camera (`devtools/soak.sh`).
- **Stop your own loops.** A soak that a task stop "ended" kept sending stick moves through its child shell and was mistaken for an input bug. Kill the shell process, and check `grep -c 'DM_CTL: press' log.txt` against what you sent.
- **Test on screen, in the app.** Headless `dmhost` runs only do for first frames. NEO had a crash only the app saw (its garbage collector scanning a guard page).
- **Measure in the same scene, before and after.** Prefer flipping a live switch within one run to comparing two runs.

## Bring-up phases and what "done" means

The steps for each phase are in `nx2android-dev/references/new-game-profile.md`. These are the device checks that close each phase:
1. **Boot to the frame loop.** Per-frame `timing up to frame` lines appear in `log.txt`. Android-specific causes NEO hit:
   - an alias region whose alignment depended on ASLR (`svcBreak` in nninitStartup);
   - an incomplete RomFS push (a null-pointer crash after a few frames);
   - a garbage collector scanning a stack guard page (SIGSEGV in the app only; fix: mmap the stack and pass it with `pthread_attr_setstack`).
2. **Correct first frames.** The logo, the title and one 3D scene match screenshots of the real game. Check docked and handheld: an even number of copies hides a wrong clip-space y convention.
3. **Correct in play.** The user has played it and reports nothing wrong. Take the user's real-Switch comparison seriously ("flicker is our bug, I've tested on switch").
4. **Speed.** The target is agreed with the user. NEO's went from "60 fps" to "30+ fps, adaptive to the display, playable".
5. **Stable.** A 30-minute `devtools/soak.sh` in gameplay shows the same process id throughout and no new `[RED]` lines, on the build you hand over. A user's play session counts too; `scripts/watch_session.sh` records it without sending input.

## Driving the device

Installing, launching and the basic control-file lines are in `nx2android-dev/references/device-loop.md`. What this skill adds:
- **`env.txt` versus `ctl.txt`.** `env.txt` switches are read at start, so restart the app after changing them. `ctl.txt` `env KEY=VALUE` lines take effect while the game runs (`references/switches.md` says which switches can).
  - The reader starts at the file's end on launch; it used to replay every old line, which walked menus and pressed A by itself.
  - Release builds ignore `ctl.txt`.
- **Scripts:**
  - `scripts/ctl.sh 'X' 'B~0.15'` appends lines in one write.
  - An unknown key such as `none~0.1` logs `[RED]` but still waits its hold, so it works as a delay between lines.
  - Presses of 0.2 s worked for NEO, but Viractal missed short ones: 0.4 s is the safe default.
  - `adb shell input tap` is too short for the game's own touch screen (use `input swipe x y x y 150` for a held touch). It is enough for the front end, whose overlay buttons, pause menu and keyboard act when the finger lifts.
- **Getting to gameplay:** use `scripts/boot_to_gameplay.sh DETECTOR.py`, with a detector written for the game (`scripts/detect_neo.py` is the example).
  - A brightness test passed on NEO's colourful title screen. Check the detector against a title and a gameplay screenshot.
  - It must pass twice in a row.
  - For release builds, set `BOOT_TAP="x y"` to tap the on-screen A button instead.
- **Logs:**
  - `log.txt` is the current run and `log.prev.txt` the run before; each launch rotates them, so read the log after a crash before relaunching twice.
  - Lines worth grepping: `timing up to frame` (frame p50/p95/max), `replay per frame` (draws, replay ms, GPU busy, descriptor reuse), `adaptive frame rate`, `passes by GPU time`, `[slowframe]` (with `DM_SLOWFRAME=1`).

## How to work a bug
1. **Make it countable first.** Turn "it flickers sometimes" into "58 of 118 frames bad" with `rec_flicker.py`, or with a colour-box count over frame shots (`scripts/count_matching_frames.py`). Every change is then a number.
2. **Toggle, don't edit.** Most backend behaviours have a live switch (`references/switches.md`). Flip one with a `ctl.txt` `env` line and count again in the same run. If a switch you need is missing, add one, read in `x_read()` in `gfx_vk_exec.c` so it changes live.
3. **Kill theories with counters.** A CPU/GPU race theory died when a counter showed 0 races in about 3M reads; a texture-aliasing theory died at 0 stale reads.
4. **Check that the tool isn't the cure.** The pass dump's own depth copy hid the brown wipe. When a debugging tool makes a bug disappear, repeat each thing the tool does on its own: that finds the fix.
5. **Pin the frame.** `DM_VK_PASSDUMP=N` waits N−1 presents. Arm it in the same `ctl.txt` write as `DM_VK_SHOTS`; the log order shows which present it caught. Expect to retry, because frame timing jitters by one.
6. **Run the validation layer before theorising** whenever the driver refuses work. NEO's "draw 712 limit" took hours of guesses; the layer named the cause in one run.

## Performance in short

Details and the cases behind them: `references/playbook.md`.
1. Read the per-frame lines first (`replay per frame`, `timing up to frame`): replay ms (CPU), GPU busy ms, draws, passes, descriptor reuse; compare at the same frame rate.
2. Check that CPU replay and GPU work overlap (`DM_NVN_SEQ=N` traces the render thread); submit in pieces (`DM_VK_CB_DRAWS`) or `DM_NVN_ASYNC=1`.
3. Cut passes and reuse descriptor sets before anything else in the backend.
4. Pacing problems: `adaptive frame rate` and `the game sets present interval` lines; pin a rate with `env DM_FPS_PIN=40` before blaming the governor.
5. Hitches left: sample per-thread CPU from `/proc/<pid>/task/*/stat`.
6. GPU profile with `DM_VK_PASS_TIMES=1`; when load/store A/B saves under 1 ms the cost is the game's own shading, and the remaining levers lower quality: the user's call.

## Measurement traps

- The CPU may be capped by policy, not heat: the Y700's `scaling_max_freq` stayed at ~2.0 GHz (of 3.5 / 4.3) at 55 °C. `dumpsys thermalservice` showed cached temperatures (93 °C) hours old: read `/sys/class/thermal/thermal_zone*/temp`.
- Dropped submissions leave stale GPU timestamps.
- One thermal sysfs zone can be stale; read several.
- The GPU clock isn't readable on retail devices; infer it from GPU busy at a fixed rate.
- `simpleperf` is refused on retail devices, and an in-process SIGPROF sampler crashed the game: use the replay's timers and per-thread `/proc` sampling.
- Runs that reach different scenes aren't comparable.
- A replay that is slow while no thread is near a full core (per-thread `/proc` sampling), with the profiler's samples on plain reads of op data (`DM_VK_PROF`: the first read of each op header in `exec_ops`), is reading slow memory. The `hardware buffers:` start-up line gives the CPU's read speed of a hardware buffer against malloc memory: a Retroid Pocket 5 reads every hardware buffer and every Vulkan memory type, HOST_CACHED included, write-combined, at 1.7-1.9 GB/s (`DM_VK_ZEROCOPY_LAZY`, references/switches.md).
- On the Retroid Pocket 5 the kgsl counters (`gpubusy`, `gpu_busy_percentage`, `gpu_clock_stats`, `devfreq/gpu_load`) stand still in its High performance mode: they read the same for minutes and say nothing. The replay's `GPU` / `GPU busy` figures are a span, from a submission's first part starting on the GPU to its last part ending: with parts sent early they include the GPU's waits for the replay. To see how long the GPU itself needs, turn the early parts off (`DM_VK_SUBMIT_SPLIT=0`, live) and read the game's wait after the submission in a `DM_NVN_SEQ` trace.
- Run analysis scripts with plain `python`, not `python -I`: they need Pillow, numpy and PyAV from user site-packages.

## Scripts in this skill

They take the device and app from `devtools/device.json` (via `devtools/device.py`), so they work for any game.
- `scripts/ctl.sh LINE...` appends lines to the running app's `ctl.txt` in one write.
- `scripts/frame_shots.sh N OUTDIR [LINE...]` sends the lines plus `env DM_VK_SHOTS=N`, waits, and pulls the N presented frames.
- `scripts/count_matching_frames.py SOURCE R0 R1 G0 G1 B0 B1 [threshold]` lists frames with more than the threshold of pixels inside a colour box. SOURCE is a folder of shots or an `.mp4`.
- `scripts/boot_to_gameplay.sh DETECTOR.py` restarts the app and presses A until the detector says gameplay, twice in a row. `scripts/detect_neo.py` is the example detector.
- `scripts/watch_session.sh MINUTES LOG` logs the process id, the latest timing line and the red-line count every minute, sending no input.
