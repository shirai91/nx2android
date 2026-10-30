# Host and backend switches (Android)

**How to set them:**
- **Start-up switches:** put them in the app's `env.txt` (`/sdcard/Android/data/<package>/files/env.txt`, one `KEY=VALUE` per line), then restart the app.
- **Live switches:** append `env KEY=VALUE` to `ctl.txt` while the game runs. These are re-read at each submission or present. `ctl.txt` needs a development build with `DM_CTL=<files>/ctl.txt` in `env.txt`; release builds ignore it.

The source of each switch is in `nxnative/dm/android/` unless noted. Check the code comment there before relying on a description below: switches are added and retired as bugs are found.

## Diagnostics

| Switch | When read | What it does |
|---|---|---|
| `DM_VK_VALIDATE=1` | start-up | Khronos validation layer; its messages go to the log. The APK must carry it (`mkapk.py --extra-so <libVkLayer_khronos_validation.so>`; built copy in `tools/vvl`) |
| `DM_VK_SHOTS=N` | live | save each of the next N presented frames as `frame_<n>.ppm` in the files folder (logs "frame shots: presents a to b") |
| `DM_VK_SHOT=n1,n2` | start-up | headless runs: save those presents (`DM_VK_SHOT_DIR`) |
| `DM_VK_PASSDUMP=1` / `=N` | live | dump each pass's first colour target and its depth (PPM, NaN counts, depth stretched) for one frame; `=N` waits N−1 presents first. **The dump adds barriers and depth reads, which can hide bugs** |
| `DM_VK_TRACE_PASS=-2` / `=n` | live | `-2` lists one submission's passes; `n` logs every draw of pass n once (targets, depth target, textures, pipeline state); the `-2` list also shows each barrier's NVN bits and where each pass was ended from (an offset in `libdmhost.so`) |
| `DM_VK_TRACE_SIZE=WxH[:fmt[:fmt]][#n]` | live | trace the (n+1)th pass whose first colour target has that size (and formats): pass numbers move by tens from frame to frame |
| `DM_VK_TRACE_FRAME=WxH[:fmt[:fmt]]` | live, one submission | trace every pass of the next submission whose targets match (a whole effect chain in one log); trace lines also describe each sampler (wrap, filters, LOD, border) |
| `DM_VK_PASSDUMP_SKIP=n` / `=WxH`, `DM_VK_PASSDUMP_SIZE=WxH[,WxH]` | read when a dump starts | skip the first n passes, or start at the first pass of that size (depth included); only passes of those sizes (colour only) |
| `DM_VK_PASSDUMP_FRAMES=N` | read when a dump starts | keep dumping for N presents in a row (up to 96 dumps; the log names each dump's present): with `DM_VK_PASSDUMP_SIZE`, catches an effect that is wrong in single frames |
| `DM_VK_PASSDUMP_DEPTH=1` | read at each pass | with `DM_VK_PASSDUMP_SIZE`, the size-filtered passes' depth too; every depth dump also writes its raw values as float32 (`<name>.f32` beside the PPM) |
| `DM_VK_TEXDUMP=<pool hex>+<offset hex>` | read when a dump starts | during the dump, the first sampled image at that pool offset (the trace prints both) is blitted to RGBA16F and dumped with the passes (level 0, up to 6 layers, pass number −100 − layer): a compressed texture such as a BC6H cube map, decoded by the GPU |
| `DM_VK_NOPCACHE=1` | start-up (`gfx_vk_core.c`) | start with an empty Vulkan pipeline cache (the saved one is ignored; app-owned files cannot be deleted over adb) |
| `DM_VK_WATCH=<pool offset hex>[#n]` | live | log the next n uses of images at that pool offset (attachment, sampled, copy) with present and pass numbers: who writes what a pass reads |
| `DM_VK_UBO_LOG=<shader key hex>[#n]` | live | log the next n uniform buffers that shader reads (every word up to 512 bytes, a hash; for register 0 of a vertex shader also its vertex stream 0; for an indexed draw, the index data's hash and range, every word of the first vertex the draw reads, and once per index data every index and vertex). Use with `DM_VK_X_NOSETC=1`, or reused descriptor sets hide draws. With a primitive-ID fragment shader through `DM_VK_SPV_ALT` it shows which triangles a draw lost (XC3's cloud dome) |
| `DM_VK_SPV_DIR=<dir>` (start-up) + `DM_VK_SPV_ALT=1` (live) | | pipelines made while `ALT` is on use `<dir>/<key>.spv` in place of the bundle's code where the file exists (same bindings), so two compilations of a shader compare in one run (e.g. the translator's GLSL through `glslc`, or a patched module) |
| `DM_VK_PASS_TIMES=1` | start-up | GPU timestamps around every pass; the per-frame line lists the costliest passes. Disturbs Adreno's binning: compare runs only |
| `DM_SLOWFRAME=1` | start-up (`gfx/nvn/hle_nvn.c`) | one `[slowframe]` line per dropped frame with a breakdown (frame, work, NVN, wait, thread and process CPU) |
| `DM_VK_LOG_UPLOADS=1` | start-up | which packaged textures are uploaded or re-uploaded |
| `DM_VK_SKIP_OPS`, `DM_VK_DBG_DRAW` | start-up | bisection aids: skip command kinds; 1 no draw calls, 2 no vertex buffers, 4 no descriptor sets |
| `DM_VK_NOCULL=1` | start-up | no face culling (test for winding bugs) |
| `DM_VK_PROF=1` / `=2` | live, arms once (`gfx_vk_exec.c`) | sampling profiler of the replay thread (1 ms of its CPU time): every 20 s the most frequent places, as offsets in `libdmhost.so` (`=1` charges libc and driver time to our first caller, shown `via:`; `=2` shows the place itself). Resolve with `llvm-symbolizer --obj=<build>/android/host-debug/libdmhost.so 0x<offset>`  **Xenoblade Chronicles 3 crashed in its own data code while loading a save in 2 of 2 runs with it on from start-up (0 of 4 without): arm it after loading** |
| `DM_VK_PROF_TID=<tid>`, `DM_VK_PROF_OUT=<file>` | read when `DM_VK_PROF` arms | sample that thread of the process instead of the replay thread, and append the raw samples (leaf pc, lr, six frame-chain returns) to the file. **Host threads only: sampling Xenoblade Chronicles 3's render thread crashed the game in its own code** (use `DM_NVN_ASYNC=1` to profile the replay on a host thread) |
| `DM_VK_CRUMBS=1` | start-up (`gfx_vk_exec.c`) | for a device lost (GPU hang): each pass end and dispatch writes its number into host-visible memory after a full barrier; when the device is lost the log names the last work the GPU finished and the passes after it, with their shaders. Slows the GPU |
| `[timeline]` lines | with `DM_NVN_ASYNC=1` (`gfx/nvn/hle_nvn.c`) | per frame: the game's gap between a sync wait and its next submission, queue delay, replay, and the GPU's tail after the replay |
| `DM_NVN_SEQ=N` | live (`gfx/nvn/hle_nvn.c`) | the next N frames' submissions, fences, acquires, sync waits (with the time waited) and presents, in order, with thread and time: what the render thread waits for |

## Behaviour toggles (for A/B; defaults are the fixes)

| Switch | When read | Default and meaning |
|---|---|---|
| `DM_VK_CB_DRAWS=N` | live | split the frame into command buffers every N draws (128), only before passes with a new depth image; `0` = never |
| `DM_VK_SUBMIT_SPLIT=0` | live | don't submit the pieces early |
| `DM_VK_X_CUTANY=1` | live | cut before any pass (the old rule that caused LRZ flicker) |
| `DM_VK_X_NOGS=1` | live | leave out draws with a geometry shader (A/B their GPU cost: Xenoblade Chronicles 3's effects, 0.7 ms of 22 in a battle) |
| `DM_VK_LRZ_TOUCH=0` | live | drop the LRZ touches: the one-texel depth read before passes that load a depth-stencil buffer (brings NEO's menu-wipe flicker back) the texel written back into the depth buffer a pass switches away from (brings XC3's cloud-layer drop-outs back), and the texel written back into a depth buffer a pass goes on with after a command-buffer cut (brings Hades II's one-frame light drop-outs back) |
| `DM_VK_FDZ=0` | live | no fragment-depth variants: draws that test a depth buffer a fragment shader wrote through `gl_FragDepth` (in the last two presents) normally get a variant of their fragment shader that writes `gl_FragDepth = gl_FragCoord.z`, the same depth tested after the shader, out of Adreno's early culling, which dropped whole triangles of Hades II's lights (they went out in turn every 24-48 frames) |
| `DM_VK_FLIPY=0` | live | the old clip-space y convention (pictures flip) |
| `DM_VK_X_NOBAR`, `X_DEPTH`, `X_COLOR` | live | cost measurements that **draw wrong**: no barrier before a pass; depth neither loaded nor stored; colour not loaded unless cleared |
| `DM_VK_X_NOSETC`, `X_NODRVC`, `X_LANDCUT`, `X_NOKEEP`, `X_NOSHC` | live | flicker experiments: no descriptor-set reuse; no driver-segment reuse; deferred uniform updates land at each cut; no pass kept for repeated targets; no uniform-shadow reuse |
| `DM_VK_DYN_UBO=0` | start-up | no dynamic uniform-buffer offsets (descriptor reuse drops) |
| `DM_VK_NOPREWARM=1` | start-up | no pipeline prewarm |
| `DM_VK_UBWC=3` | start-up (`gfx_vk_tex.c`) | default 3 (bits): 1, colour images get a format list (their format and its sRGB twin) instead of MUTABLE_FORMAT; 2, depth images neither MUTABLE_FORMAT nor EXTENDED_USAGE. Adreno then keeps its UBWC compression: Super Mario Bros. Wonder's world map 11.8 → 8.7-8.9 ms of GPU (9.5-10.0 with 1 alone), Unicorn Overlord 35-40% less on the Adreno 730. A colour view in another format falls back to the image's own and logs `[RED] view format … of a format-listed image`: such a game wants `2` (or `0`) in its profile |
| `DM_VK_SYNC_PRESENT=1` | start-up | present on the game thread instead of the present thread |
| `DM_NVN_ASYNC=1` | start-up (`gfx/nvn/hle_nvn.c`) | asynchronous replay: submissions and presents run on a replay thread, syncs wait for the job's replay and then its GPU fence. Xenoblade Chronicles 3 has it in its profile (`env`): the replay (~26 ms a frame in Resolution-mode battles) overlaps the game's render thread; the window acquire then waits for every job queued so far to be replayed (`DM_NVN_ACQ_REPLAYED`), and texture registrations are seen by each job as they were when it was queued (`gfx_vk_tex.c`). Off by default; `[timeline]` lines show the frame |
| `DM_NVN_ACQ_REPLAYED=0` | start-up (`gfx/nvn/hle_nvn.c`) | with `DM_NVN_ASYNC=1`: the window acquire does not wait for the queued jobs' replay. XC3 then drew whole-screen garbage (solid colours, smears) while the camera turned in about half of the recordings; with the wait at the next submission instead, 2 of 6 |
| `DM_VK_X_NOPRERESET=1` | start-up (`gfx_vk_core.c`) | a submission slot's command buffers are reset as they are begun again, on the replay thread, instead of on the completion thread when its submission is done |
| `DM_NVN_ACQ_NOW=1` | live (`gfx/nvn/hle_nvn.c`) | the window acquire's sync fenced at all work so far (the old way) instead of at the texture's last present: Xenoblade Chronicles 3 then waits for its whole frame on the GPU before it submits the next |
| `DM_VK_X_NOBC=1` | live | every draw sets all its dynamic state and binds again (no bind cache; the cache starts empty in each command buffer and each render pass) |
| `DM_VK_RD_TOUCH=0` | live | no transfer read of the memory a run of rasterizer-discard draws wrote, where the run ends: Xenoblade Chronicles 3's grass then flickers (whole blades black in some frames: the G-buffer's grass cards, depth EQUAL against the pre-pass, meet sway records not yet written) |
| `DM_VK_X_RDKEEP=1` | live | runs of rasterizer-discard draws (vertex shaders that write memory) stay in the pass of the draws around them instead of a pass of their own: Xenoblade Chronicles 3 then draws thin black slivers over cliffs and grass in a few percent of frames |
| `DM_VK_X_SKIPVS=<key>[,<key>...]` | live | leave out the draws whose vertex shader key (hex, as the traces print it; up to 8) is listed: bisect which shaders an artifact needs, a group at a time (it found XC3's slivers from 40 candidates in 4 rounds) |
| `DM_VK_RP=0` | start-up | dynamic rendering instead of cached `VkRenderPass` objects with imageless framebuffers (Adreno's `vkCmdBeginRendering` cost about 40 µs a pass); ignored where the driver gives no dynamic rendering |

## Frame pacing and modes

| Switch | When read | Meaning |
|---|---|---|
| `DM_FPS_PIN=60/40/30` | live | hold that frame-rate step; any other value lets it adapt again |
| `DM_ADAPTIVE_FPS=0` | start-up | keep the game's own period |
| `DM_VK_PACE=0` | live (every 30 presents, `gfx_vk_present.c` `pace_desired`) | present without desired times (`VK_GOOGLE_display_timing`): frames go out at the first vsync after they are latched, as before. On, each frame is timed at the last one's plus the game's period on the display's vsync grid; the log's `[perf] display timing` line counts resyncs (a longer frame where the cadence restarted later) and frames shown a vsync or more after their time. Measure on screen with `dumpsys SurfaceFlinger --latency "<the app's NativeActivity#N layer>"` (column 2 = actual present) |
| `DM_MOVIE_AUDIO_SLOTS=N` | first audio decoder (`host/hle_movie.c`) | profile env: fewer output slots for the movie library's audio decoders than 64 (4..63), for a player that keeps the outputs it acquires in a fixed ring (Mario vs. Donkey Kong's ring holds 32) |
| `DM_NO_ADPF=1` | start-up | no Android performance hints |
| `DM_OPMODE=0/1` | start-up (`host/hle_oe.c`) | handheld / docked at launch. The pause menu's Mode setting (`docked=` in `settings.txt`) overrides it and switches live |
| `DM_AUDIO=0` | start-up (`host/hle_audio.c`) | silent sink (Android defaults to real output) |
| `DM_SHAKE_G=1.5` | start-up (`android/front_android.c`) | how hard (in G beyond gravity, 1.5 by default) the device must be shaken for the controller's six-axis sensor to be shaken (`DM_BTN_SHAKE`, `host/hle_hid.c`; Super Mario Odyssey's cap throw). Each detected shake logs `front end: device shaken (peak … G)`; `ctl.txt` `Shake~0.4` shakes it without the device |
| `DM_TOUCH_DOCKED=screen` | first touch read (`host/hle_touch.c`) | profile env: docked touch points as a game that takes the panel's values as screen pixels expects them (Unity 2019.4: x = screen column, y = screen row − 360 on 1920x1080). On Android touches reach the game in both modes (`hle_hid.c` `touch_on`); a Switch has no docked touch |

## Control file lines (`ctl.txt`)
- **Buttons:** `A`, `B`, `X`, `Y`, `L`, `R`, `ZL`, `ZR`, `Plus`, `Minus`, `Up`, `Down`, `Left`, `Right`, `LS`, `RS`.
- **Sticks:** `LUp`, `LDown`, `LLeft`, `LRight` (same for `R…`).
- **Hold and combine:** `~seconds` sets the hold (default 0.15), e.g. `A~0.2`. `+` combines keys.
- **Touch:** `T<x>x<y>` taps a normalized point.
- **Motion:** `Shake` shakes player 1's controller (its six-axis sensor) for the hold, at least one whole shake.
- **Other lines:** `env KEY=VALUE`, `shot <path>`, `passdump <n>`, `drag x0 y0 x1 y1 [s]`. A line starting with `#` is ignored.
- **Delays:** an unknown key name logs `[RED]` but still waits its hold, so `none~0.1` works as a delay between lines.
- **Replay:** the reader starts at the file's end when the app launches; lines from earlier runs are never replayed.
