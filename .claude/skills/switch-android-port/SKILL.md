---
name: switch-android-port
description: Track down and fix problems in a Switch game running on an Android device under nx2android - crashes in the app, rendering bugs (flicker, vanishing blocks, upside-down pictures, refused Vulkan submissions, wrong colours), stutter and frame rate, soak tests and play-session checks - with the methods that found NEO - The World Ends with You's bugs (screen recordings, frame shots, pass dumps, live A/B switches, the validation layer). Use when chasing a symptom on the tablet or measuring speed or stability there. For where a change belongs (profile or core), adding a game profile, the port pipeline and the repository's rules, use nx2android-dev; the two are meant to be used together.
---

# Bringing a port up on the device

This skill is about the device side of a port: making a problem on the tablet countable, finding its cause, and proving a fix. It holds the working method and the playbooks from bringing up NEO: The World Ends with You, with the bugs that cost the most time.

**What `nx2android-dev` covers instead:**
- deciding profile or core;
- adding a game profile and the port pipeline;
- installing and launching (`references/device-loop.md`);
- repository rules, tests and docs;
- the Windows environment.

Read it alongside this one. Fixes found with this skill go where `nx2android-dev` says: general rules in the core, per-game facts in the profile.

Other references:
- `references/neo-case-study.md`: what happened with NEO, phase by phase and bug by bug, with numbers and dead ends.
- `references/switches.md`: every host and backend switch, and whether it can be changed while the game runs.
- `nxnative/docs/agents/03-bringup.md` and `05-debugging.md`: upstream's failure catalogue and debugging recipes. They were written for Apple, but the host and rewriter parts apply here too.

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

## Debugging playbook

| You see | First move | Causes found so far |
|---|---|---|
| Whole frames dropped; `VK_ERROR_OUT_OF_HOST_MEMORY` at `vkEndCommandBuffer`; looks like a fixed draw limit | build with the Khronos validation layer (`mkapk.py --extra-so tools/vvl/.../libVkLayer_khronos_validation.so`, `DM_VK_VALIDATE=1`) | an image view made for sampling reused as a render target (cache key without usage). Bisecting draws on the device found *where*, never *why* |
| Thin black slivers (triangles with a corner thrown far away) over the scene in a few percent of frames, different every frame | count them in recordings; check the G-buffer depth under them (nearer than the scene: real geometry); then bisect with `DM_VK_X_SKIPVS`, groups of shaders at a time | XC3: vertex shaders with rasterizer discard write the transformed meshes inside the G-buffer pass; Adreno's binning pass let later draws read them unwritten. Fix: a run of such draws gets a pass of its own (`DM_VK_X_RDKEEP=1` A/B). Splitting each draw into its own pass broke Noah's hair: keep a run together |
| Grass, foliage or other geometry drawn twice (a depth pre-pass, then colour with depth EQUAL) shows black holes in whole blades, different every frame | count G-buffer pixels with depth but no albedo in pass dumps (`DM_VK_PASSDUMP_DEPTH=1`); skip each pre-pass writer with `DM_VK_X_SKIPVS` to find whose depth they are; then draw the colour pass from a copy of its vertices | XC3: both passes draw from records that rasterizer-discard vertex shaders write just before; barriers did not stop Adreno's next passes from reading them unwritten (identical records, instances and shader code; a GPU wait after each submission did not help). Fix: a transfer read of the written memory where the run ends (`DM_VK_RD_TOUCH=0` A/B); a command-buffer cut also worked |
| Blocks of near objects vanish or change colour for a frame | record the screen (`devtools/rec_flicker.py`) and count bad frames; screenshots miss it | Adreno's low-resolution depth (LRZ) going stale: (1) at command-buffer cuts between passes sharing a depth buffer, (2) when a pass reloads a depth-stencil buffer in the same command buffer. Fixes: cut only before passes with a new depth image; a one-texel depth read before load passes (`lrz_touch`) |
| Whole triangles of one draw missing, straight edges along its mesh, while the camera moves (and at some still angles) | pass dumps over several frames with a fragment shader that writes `gl_PrimitiveID` (`DM_VK_SPV_ALT`): which triangles went, and whether one with `gl_FragDepth` written (no LRZ) loses any | Adreno applied the LRZ of another depth buffer in the binning of a later pass (XC3's cloud dome on its own D32 buffer). Fix: the depth buffer a pass switches away from gets a texel written back (`lrz_touch` in `pass_commit`); a read alone did not drop that LRZ |
| Lights or other depth-tested draws go out for a few frames at a time, at a steady rhythm, with the same draws, state and depth contents every frame (UBO_LOG, per-frame pass traces, depth dumps); whole halves of quads missing in pass dumps | A/B `DM_VK_FDZ=0`. A live switch that drops the depth test for draws that write no depth tells whether the depth hardware is involved | Adreno's early culling on a depth buffer a fragment shader wrote through `gl_FragDepth` (Hades II: Blit2DDepth downsamples the scene depth, then its lights test it). Writing the depth back by transfer and serializing the GPU did not help; the light shader writing `gl_FragDepth = gl_FragCoord.z` did, which is now the rule (fdz in `gfx_vk_exec.c`) |
| Lit areas go dark for one frame while walking, only with command-buffer cuts (`DM_VK_CB_DRAWS=0` clears it) | count one-frame dark spikes over recordings, A/B `DM_VK_CB_DRAWS`, `DM_VK_SUBMIT_SPLIT`, `DM_VK_LRZ_TOUCH` | a pass going on, after a cut, with a depth buffer drawn before it (Hades II): such a pass gets a texel of its depth written back first |
| A 1–2 frame glitch in a transition | `scripts/frame_shots.sh N DIR '<trigger line>'`: is it in the game's output, or added at presentation? | the second LRZ case above |
| Fog everywhere, missing ground, dark silhouettes | pass dumps (`DM_VK_PASSDUMP`, incl. depth); try `DM_VK_NOCULL=1` | inverted front-face winding culled the ground, so depth was empty and the fog maxed out |
| One surface black after tonemapping (NaN in its pass), fixed by clearing NaN from one varying | a fragment-shader probe that colours that varying by NaN / Inf / zero; then `DM_VK_VATTR_LOG=<vertex shader key>` for the attribute behind it | Dead or Alive Xtreme 3 Scarlet's sea: shader_vk.py `int_inputs` declared an RGBA8 UNORM attribute the vertex shader only copied (float -> int register -> float) as sint; R8G8B8A8_SINT made bytes >= 0x80 NaN bit patterns. A raw move now leaves an input float. A `DM_VK_SPV_ALT` module cannot test this: the input formats follow the bundle's reflection |
| Picture upside down in one mode only | pass dumps of each copy in that mode's chain | wrong clip-space y convention: NVN's upper-left mode is y-up clip space, so use a negative-height viewport with the scissor unflipped |
| Shadows detached from characters, distant people flicker | check deferred uniform updates and their shadow copies | a shadow mixed the newest update with memory a still-running GPU copy hadn't written yet |
| Flat odd colour where a texture should be | `[alias] stale read` lines (textures over the same memory with other descriptions are separate images). The fallback textures are black, so a non-black flat colour isn't the fallback | in NEO this was the LRZ case, not aliasing |
| Frames never appear; every present waits 1 s | present paths that return early | the present ticket must complete on every path |
| Every draw skipped as "no program" | replay counters | NVN binds programs by stage mask: replace only the masked stages |
| A model drawn as a flat black silhouette; `shader … is not in the bundle` lines | pass dumps: depth there, G-buffer albedo black = its G-buffer draw was skipped | shaders whose code is in no game file (Xenoblade Chronicles 3: effects and model materials). The app captures them (`captured_shaders/`); port again with `--install` to translate them in |
| No sound | "silent sink" in the log | real output defaulted on for iOS only |
| Crash in the audio device callback, or "more than 8 AudioOuts open", after the pause menu's mode switch | count `OpenAudioOut` against `Close` lines | the game closes an output only when its `AudioOut` struct is marked open (first word non-zero, as the SDK leaves it): unmarked, every mode change opened another one beside the old |

How to work a bug:
1. **Make it countable first.** Turn "it flickers sometimes" into "58 of 118 frames bad" with `rec_flicker.py`, or with a colour-box count over frame shots (`scripts/count_matching_frames.py`). Every change is then a number.
2. **Toggle, don't edit.** Most backend behaviours have a live switch (`references/switches.md`). Flip one with a `ctl.txt` `env` line and count again in the same run. If a switch you need is missing, add one, read in `x_read()` in `gfx_vk_exec.c` so it changes live.
3. **Kill theories with counters.** A CPU/GPU race theory died when a counter showed 0 races in about 3M reads; a texture-aliasing theory died at 0 stale reads.
4. **Check that the tool isn't the cure.** The pass dump's own depth copy hid the brown wipe. When a debugging tool makes a bug disappear, repeat each thing the tool does on its own: that finds the fix.
5. **Pin the frame.** `DM_VK_PASSDUMP=N` waits N−1 presents. Arm it in the same `ctl.txt` write as `DM_VK_SHOTS`; the log order shows which present it caught. Expect to retry, because frame timing jitters by one.
6. **Run the validation layer before theorising** whenever the driver refuses work. NEO's "draw 712 limit" took hours of guesses; the layer named the cause in one run.

## Performance playbook

1. **Read the per-frame line first:** replay ms (CPU), GPU busy ms, draws and passes, descriptor reuse. GPU busy depends on clocks, so compare at the same frame rate.
2. **Check that the frames overlap at all.** If a frame takes replay + the GPU's whole tail, trace the render thread with `DM_NVN_SEQ=N`: Xenoblade Chronicles 3 waited in `SyncWait` on its window-acquire sync, which the host fenced at all work so far instead of at the texture's last present; and every submission once took submission slot 0 and waited for the one before on the GPU (fixing that took its docked frames from 37 to 33 ms).
3. **Overlap the CPU replay with the GPU.** Submit in pieces (`DM_VK_CB_DRAWS`, every 128 draws, between passes only, respecting the LRZ cut rule). NEO went from 36.7 to 15.7 ms. When the render thread is still the limit (replay plus the game's own work), `DM_NVN_ASYNC=1` moves the replay to a thread of its own; check it with turning recordings, not single shots: XC3 drew whole-screen garbage in about half of them until the window acquire waited for the queued jobs' replay (and texture registrations were versioned by job), then 0 in 20.
4. **Cut passes.** Defer uniform updates instead of ending the pass, and keep the pass when the same targets are set again. NEO went from 145 passes for 148 draws to 28. Not across NVN barriers: keeping XC3's G-buffer pass over its fragment- and primitive-order barriers tore its characters in some frames (storage written by rasterizer-discard vertex draws).
5. **Reuse descriptor sets.** Dynamic uniform-buffer offsets took NEO from 6% reuse to 86%, plus a per-submission texture-view cache.
6. **Frame pacing** (`perf_android.c`):
   - The governor steps 60/40/30 fps and matches the display rate. It judges p90 frame work after 4 s at a new rate, because clocks take seconds to ramp.
   - It steps down only when frames also miss the display: at least 7 of the last 64 shown longer than 1.5 periods, read from the presentation times (`late on screen` in its log line). Work a little over the period still makes its vsync through the swapchain's queue: Viractal at 60 stepped to 40 on p90 work of 17.5 ms while it dropped 0-10 of 300 frames with the rate held. It steps back up when p90 work fits the shorter period at the current clocks (no 15% margin: at 40 the clocks drop with the slack, and Viractal's board read 15.6 ms at 40 but held 60 with 0 frames late); a step up that does not fit shows on screen and steps back down, with the next try waiting twice as long.
   - A frame is late against the longer of the step's period and the hold the game asked for (its present interval). Super Mario Bros. Wonder runs sead's dynamic interval controller (main+0xcfd24): it sets interval 2 for the frame after one whose main-loop span ran long, then 1 again; in DM_NVN_SEQ traces the requests followed frames where its wait on the last frame's GPU fence ran 5-9 ms (our replay plus the GPU tail after it). Counted as late, those 33.3 ms frames stepped the rate to 40, which made the spans longer still. Grep `the game sets present interval` before blaming the governor: a game toggling it many times a second is reacting to replay + GPU tail, and the cure is making that shorter.
   - Keep the ADPF target at the game's own period.
   - Make sure frame start is recorded on every present path: NEO's governor never ran until it was.
7. **Threads:** if hitches remain, sample per-thread cores and CPU clocks over adb (`/proc/<pid>/task/*/stat`, cpufreq). NEO's main thread sat outside the ADPF session on half-clocked mid cores while the prime cores idled; the busiest threads now join the session every 300 frames.
8. **Test a rate directly before blaming the port.** `env DM_FPS_PIN=40` holds a rate. NEO's crowd held 40 fps pinned (GPU 20.6 → 15.2 ms once clocks rose), which showed the governor was giving up too early.
9. **GPU profile** with `DM_VK_PASS_TIMES=1` (start-up only; it disturbs Adreno's binning, so compare runs).
   - If the load/store A/B (`DM_VK_X_DEPTH`, `DM_VK_X_COLOR`) saves under about 1 ms, the cost is the game's own shading.
   - The only levers left then lower image quality (render resolution, shader precision). That is the user's call; say so rather than promise 60 fps.

## Measurement traps

- The CPU may be capped by policy, not heat: the Y700's `scaling_max_freq` stayed at ~2.0 GHz (of 3.5 / 4.3) at 55 °C. `dumpsys thermalservice` showed cached temperatures (93 °C) hours old: read `/sys/class/thermal/thermal_zone*/temp`.
- Dropped submissions leave stale GPU timestamps.
- One thermal sysfs zone can be stale; read several.
- The GPU clock isn't readable on retail devices; infer it from GPU busy at a fixed rate.
- `simpleperf` is refused on retail devices, and an in-process SIGPROF sampler crashed the game: use the replay's timers and per-thread `/proc` sampling.
- Runs that reach different scenes aren't comparable.
- Run analysis scripts with plain `python`, not `python -I`: they need Pillow, numpy and PyAV from user site-packages.

## Scripts in this skill

They take the device and app from `devtools/device.json` (via `devtools/device.py`), so they work for any game.
- `scripts/ctl.sh LINE...` appends lines to the running app's `ctl.txt` in one write.
- `scripts/frame_shots.sh N OUTDIR [LINE...]` sends the lines plus `env DM_VK_SHOTS=N`, waits, and pulls the N presented frames.
- `scripts/count_matching_frames.py SOURCE R0 R1 G0 G1 B0 B1 [threshold]` lists frames with more than the threshold of pixels inside a colour box. SOURCE is a folder of shots or an `.mp4`.
- `scripts/boot_to_gameplay.sh DETECTOR.py` restarts the app and presses A until the detector says gameplay, twice in a row. `scripts/detect_neo.py` is the example detector.
- `scripts/watch_session.sh MINUTES LOG` logs the process id, the latest timing line and the red-line count every minute, sending no input.
