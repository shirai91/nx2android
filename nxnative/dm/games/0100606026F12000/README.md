# FINAL FANTASY RESONANCE (`0100606026F12000`) on Android

Update 1.0.1 (v65536), Unreal Engine 5.6.1 (the desktop deferred renderer) on SDK 20.5.17. Measured on a POCO F7 (25053PC47G: Snapdragon 8s Gen 4, Adreno 825, Android 16), 2026-11-01, branch `ffr-android` (PR #41).

The game no longer crashes, holds its own 30 fps in normal areas, and runs the heavy scene after the first field at about 10 fps (1–3 before). With three further changes (EarlyZPass off, VelocityOutputPass to base pass, descriptor caching) the heavy scene is expected above 15 fps; device results pending.

| | Before | Now |
|---|---|---|
| Heavy scene after the first field | killed or aborted within ~40 s, 1–3 fps | stable, ~10 fps (frames 80–90 ms) |
| Normal areas (the way to that scene) | | 30 fps: 0 of 300 frames dropped, frame work p50 34 ms |
| GPU memory, heavy scene | up to 6.5 GB | 1.8 GB |
| Process mappings, heavy scene | up to 65,762 (the kernel's limit) | ~18,000 |

## The crash

Two causes, both in the Vulkan backend, both measured on the device:

1. **Descriptor pools.** Every pool had one size for any layout (4,096 sets, 65,536 combined image samplers, 32,768 uniform buffers, …). A pool takes its whole size in driver memory, used or not: on Adreno 825 a combined image sampler is ~760 bytes and a uniform buffer ~250, so one pool was ~76 MB. Each of the 16 submission slots kept the pools of its heaviest submission: 63 pools, 4.6 GB, and the system killed the app (`lowmemorykiller`, GfxDev 6.5 GB).
   Fix (`37de2d7`): pools per bank of layouts with about the same needs, 256 sets of the bank's needs each, as Eden (yuzu) makes them; a recycled slot keeps one pool per bank.
2. **The process's mapping limit.** Adreno's driver maps each 16 KB block of command memory on its own, and a command buffer reset keeps its blocks. A heavy submission took ~3,400 blocks (~55 MB, ~6.6 KB a draw); the slots in flight took the process to the kernel's ~65,500 mappings (42,569 of them 16 KB driver blocks), and every `mmap` failed: the Unreal heap ("out of memory"), scudo ("internal map failure"), the driver (`bad_alloc`).
   Fix (`37de2d7`, `69be62f`): a thread reads `/proc/self/maps` every second; over 30,000 mappings the ring is 6 slots (back to 16 under 15,000), over 45,000 a slot's command pool is released as it is reused. Descriptor buffers (below) removed most of that command memory: the heavy scene now stays at ~18,000 mappings with 16 slots.

What told them apart: `dumpsys gpu --gpumem` for the true GPU memory, a count of live Vulkan allocations against it (pools and our allocations stayed at ~1.1 GB while the GPU total grew), descriptor pool counts against GPU memory, and `/proc/<pid>/maps` (through `run-as`) for the mapping count and the 16 KB blocks. Small probes at start-up (since removed) measured the cost of each object: pools and sets add almost no mappings; 20,000 recorded buffer copies add 776 and a pool reset with `RELEASE_RESOURCES` takes them back.

## Speed: what each change bought (heavy scene)

| Change | Commit | Effect |
|---|---|---|
| `DM_VK_SETSPLIT=1`: bind only the descriptor sets that changed | `69be62f` | binds were 48% of the replay thread; slow submissions 300–390 → 190–250 ms |
| Ring of 6 slots near the mapping limit, release only past 45,000 | `69be62f` | releasing at every reuse cost 23% of the replay thread (the driver mapped every block again); slow submissions → 60–90 ms, frames 500–930 → 180–215 ms |
| Vulkan commands recorded on a worker thread (`DM_VK_WORKER`), as Eden's Scheduler | `0f9ddd6` | ~40% of the replay thread was inside Adreno's driver; the heavy submission 80 → 56 ms, frames p50 171 → 162 ms (6 slots still made each frame wait on the GPU) |
| Descriptor buffers (`DM_VK_DESCBUF`, `VK_EXT_descriptor_buffer`) | `05d0685` | no per-bind copy into the command stream: 16 slots stay, GPU memory 1.8 GB, the GPU's frame 80 → 66 ms, frames → 80–90 ms (~10 fps) |
| `r.EarlyZPass 0` patched at registration (Android only) | pending | removes the ~2,624-draw depth prepass: saves ~18–22 ms GPU (no second binning pass for 2,624 meshes), ~18 ms CPU replay |
| `r.VelocityOutputPass 1` patched at registration (Android only) | pending | merges velocity into the base pass instead of a separate depth-pass split: removes the third 2,624-draw pass, saves ~15–20 ms GPU |
| `vkGetDescriptorEXT` result cache per thread (1 024-entry direct-mapped) | pending | repeated identical descriptors across draws served from cache: saves ~8–10 ms of the ~14 ms descriptor-write cost |

Descriptor buffers on Adreno 825: offset alignment 256 bytes; descriptor sizes uniform buffer 64, storage buffer 192, combined image sampler 128, storage image 64, texel buffers 64. A buffer holding combined image samplers is reached only 128 KB at a time from one binding (`maxSamplerDescriptorBufferRange`; sampler address space 128 MB), so the 64 MB ring is bound as a window that moves with the writes, and a set outside it is copied forward (`gfx_vk_descbuf.c`).

## Why the heavy scene is still slow

The game draws a lot there, and both of this port's weak points are per draw:

- About 9 submissions a frame; the heavy one has **~8,300 draws**. ~7,900 of them are **the same 2,624 objects drawn three times**, in three passes into the same targets (colour formats 122 + 64, depth 129). Each pass begins at the game's own render-target change (`set_rt`); the backend adds no pass splits.
- Many of those draws are real geometry: ~1,000–1,800 vertices each for the busiest shaders.
- **CPU:** the replay translates every draw (collect its textures, buffers and samplers, write descriptors, record commands): ~7 µs a draw, ~61 ms for the heavy submission, of which ~42 ms is descriptors (collecting ~25, writing ~14, hashing ~4).
- **GPU:** ~66–72 ms a frame. Adreno is a tiling GPU: each pass runs its vertex work for binning and again per tile, ×3 passes.
- **Not pixels:** docked (1920×1080, 2.25× the pixels of handheld) and a 50% resolution floor (640×360 instead of 768×436) left the GPU's frame at ~70 ms.

On a Switch the same frame is cheap: the game writes NVN commands with almost no per-draw cost, and the Tegra GPU is immediate-mode.

## What was tried and did not help

| Tried | Result | Why |
|---|---|---|
| Descriptor sets cached across submissions | dropped before writing it | only ~2% of a submission's new sets were in the last 20 submissions (exact content); offsets of the uniform data change every frame |
| `-DPCVars=` / `-ExecCmds=` (also through `rom:/UECommandLine.txt`, which is where Unreal on Switch reads its command line) | ignored | the shipping build takes no console variables from the command line |
| `r.EarlyZPass 0` written in memory (`DM_CTL cvari`) | no change | the prepass mode is cached; it is re-read through the variable's change callback, which a memory write skips |
| `r.VelocityOutputPass 1`, `r.ShadowQuality 0`, `r.AmbientOcclusionLevels 0` in memory | no change | not those passes, or not read every frame |
| `r.DynamicRes.Min/MaxScreenPercentage` 50 | took effect (640×360), felt a little faster, GPU unchanged | the scene is not pixel-bound |
| 10 slots instead of 6 near the mapping limit (before descriptor buffers) | worse: 53,000 mappings, emergency releases, slower | the command memory in flight |
| The sampling profiler armed at start-up | the Unreal heap ran to 17 GB before the title | its signal every 1 ms; armed live in the scene (`ctl.txt`) it works |
| `DM_VK_PASS_TIMES=1` | frames 650–1,260 ms | its timestamp reads stall the replay; good for proportions only |
| `DM_VK_NO_ZEROCOPY=1` | segfault at frame 9 | a write into a copied pool's page; a separate bug |

Console variable objects in `main` 1.0.1 (found from their UTF-16 names and the static constructors that register them; slot 1 int, 2 float): `r.EarlyZPass` 0x8b7a128, `r.DynamicRes.MinScreenPercentage` 0x8be80d0, `r.DynamicRes.MaxScreenPercentage` 0x8be80e8, `r.Shadow.CSM.MaxCascades` 0x8b7f540, `r.ShadowQuality` 0x7c13040, `r.AmbientOcclusionLevels` 0x8affc30, `r.VelocityOutputPass` 0x8b9cef0, `r.SeparateTranslucency` 0x8b7f4b0, `r.SSR.Quality` 0x8b87468, `rhi.SyncInterval` 0x8adca10. `DM_UE_CVARS` (`hle_fpslock.c`) holds such a variable at a value from the profile, for this main build only.

## Options

| Option | Side | Expected gain | Effort |
|---|---|---|---|
| ~~Find what the three 2,624-draw passes are (trace their shaders) and turn one off in game memory~~ | both | ~~up to a third of the heavy submission~~ | done: `r.EarlyZPass 0` + `r.VelocityOutputPass 1` patched at registration; removes prepass and merges velocity pass |
| ~~Cache `vkGetDescriptorEXT` results, faster collection~~ | CPU | ~~10–20 ms a heavy submission~~ | done: 1 024-entry thread-local direct-mapped cache in `vk_db_write` |
| Record passes on several cores (state snapshot per pass, secondary command buffers) | CPU | ~3× on the replay | large |
| Bindless textures (one descriptor heap, shaders index it) | CPU | most of the ~42 ms of descriptors, for every game | largest: the shader translator's output and layouts |
| Let Adreno render geometry-heavy passes straight to memory (no binning) | GPU | unknown | experiment: Vulkan has no direct switch |
| `spirv-opt -O` on the translated shaders at port time | GPU | 10–30% where shader-bound | medium |
| 60 fps in normal areas (`DM_FPS_TARGET=60`, as upstream) | | normal areas hold 30 with frame work p50 34 ms: little headroom | small to try |

Without changing what the game draws, this phone's ceiling for the heavy scene is probably in the low-to-mid teens. With the EarlyZPass and VelocityOutputPass patches the draw count falls from ~8,300 to ~2,624 + ~400 other draws; the ceiling rises to ~15–20 fps (pending device measurement).

## Open

- An icon was reported barely visible (2026-11-01, docked, with `DM_UE_CVARS` and descriptor buffers on): which icon, and whether it was so before descriptor buffers, is not known yet.
- After hours in the heavy scene the GPU's frame crept from 66 to 75–80 ms: thermal throttling; let the phone cool before comparing numbers.

## Switches and tools used

- Profile `env` lines: `DM_VK_SETSPLIT=1`, `DM_VK_WORKER=1`, `DM_VK_DESCBUF=1`, `DM_FPS_TARGET=30`. The 50% resolution floor (`DM_UE_CVARS=84e182f72ee1da4f477804bd05b54c15:0x8be80d0=f50`) was tried and left out: no gain in the heavy scene.
- Measuring: `DM_VK_PROF_SLOW=1` (slow submissions by part), `env DM_VK_PROF=2` in `ctl.txt` (sampling profiler of the replay thread; `DM_VK_PROF_OUT` for raw samples), `env DM_VK_TRACE_PASS=-2` (one submission's passes and where each ended), `cvari` / `cvarf` in `ctl.txt` (an Unreal console variable in memory), `DM_FSLOG` (every file the game opens), `dumpsys gpu --gpumem`, `run-as … cat /proc/<pid>/maps`.
- `DM_VK_MAPS_SLOTS` (start-up): the small ring's size near the mapping limit (6).
