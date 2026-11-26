<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->
# Rune Factory: Guardians of Azuma

![Rune Factory: Guardians of Azuma](../../../../docs/images/rune-factory-guardians-of-azuma.jpg)

| | |
|---|---|
| Status | **experimental** |
| Title ID | `01003AF0200B0000` |
| Tested with nx2android | `v0.5.0` |
| Tested build and device | v589824 AYN Odin 2 (Snapdragon 8 Gen 2, Adreno 740, Android 13, Turnip) |
| App package | `org.nx2a.runefactorygoa` |

The profile sets these for you (`android.tsv`); a player changes the pause menu ones in the game, and the env rows in the data pack's `env.txt`:

| Row | What it does |
|---|---|
| `DM_VK_FDZ=0` | no fragment-depth variants: draws that test a depth buffer a fragment shader wrote through `gl_FragDepth` (in the last two presents) normally get a variant of … |
| `DM_NGC_CONTENT_VERSION=1` | nn::ngc::ProfanityFilter::GetContentVersion result; unset: 0 |
| `DM_NGC_CHINA_PASSTHROUGH=1` | nn::ngc::MaskProfanityWordsInTextForPlatformRegionChina leaves the text as it is instead of filtering through ngct:u, which the host does not serve (abort … |
| `TU_DEBUG=sysmem` | - |
| `DM_VK_X_FENCES=1` | submissions signal a ring of fences instead of a timeline semaphore, as on a Vulkan 1.1 driver without timeline semaphores (the Adreno 618 / 619 of 2021, issue … |
| `DM_GPU_TURBO=1` | "auto" or 1: ask the GPU for its high clocks while the game is GPU-bound; "need": only while the game is held below its target; unset or 0 = no |

Read the profile's porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list](../../../../README.md#games).
<!-- end of the game facts -->

## Game version

- Update 1.1.4 (v589824), with the downloadable content installed. Other versions are untested.

## Frame rate

- **Handheld mode and the game's 30 fps setting are highly recommended on mid-range devices** (such as the Odin 2,
  Snapdragon 8 Gen 2). With them and the profile's `DM_GPU_TURBO=1`, Spring Village holds 30 fps: about 29 ms of GPU
  work a frame, frame work p50 28.9 ms and p95 29.3 ms, no long frames.
- Docked: in the Spring Village shrine courtyard the Odin 2 now holds 30 fps at the full 1920x1080 (frame time p50 about
  33.5 ms, p95 38-40 ms; GPU time p95 about 33-35 ms). Earlier builds measured 21-25 fps there. The game is still close to
  GPU-bound in Docked mode, so heavier scenes can drop below 30 fps.
- `DM_VK_X_FENCES=1` stops the CPU from waiting for the GPU on every submission (on Turnip, a submission that signals the
  timeline semaphore blocks until the previous one finishes). Title scene, Docked: 18.3 ms to 0.2 ms of submit time and
  25.9 ms to 19.7 ms of frame work a frame. In Spring Village the frame time now follows the GPU time (about 39 ms against
  about 35 ms), so the play still feels slow there.
- For smoother play, a Snapdragon 8 Gen 3 or newer is recommended. On the Odin 2 (Snapdragon 8 Gen 2) Docked mode has little GPU headroom at 30 fps.
- Game speed follows real time, not the frame rate, so the game does not slow down when the frame rate drops.
- Handheld mode is faster (about 25 fps outdoors against about 21.5 fps Docked) at a lower render resolution.
- The port stays at 30 fps by default. Users who want a higher frame rate can unlock 60 fps with
  `porter port --fps60`, but the game does not hold it: it drops many frames and the frame pacing is uneven. On the Odin 2
  (Handheld, Spring Village, camera turning) it settles near 40 fps (frame time p50 25 ms, p95 31 ms). The CPU's submit and
  present work, about 21 ms a frame, is the limit. For smooth play, keep the 30 fps default.
- `gpu_tick_rate 614.4` (`game.tsv`): the game's renderer reads GPU timestamps at 614.4 MHz. Without the row it reads its GPU
  time 32 times too short. With it, the same 60 fps test reached 40 fps sooner and used less GPU time a frame (p50 about 19 ms
  against 21-23 ms), at the same 680 MHz GPU clock. At the default 30 fps the row does not make the game lower its render
  resolution: with and without it, the main passes render at the full 1280x720 in Handheld and 1920x1080 in Docked, and the
  picture stays as sharp. Both builds hold 30 fps in Spring Village. Handheld measures the same with and without the row
  (GPU time p95 about 27-30 ms). In Docked the row gives slightly more headroom (GPU time p95 about 33 ms against 35 ms,
  frame time p95 about 37.7 ms against 39.5 ms), within the scene's variance.
- `DM_GPU_TURBO=1` holds the GPU at its highest clock. It uses more power; `DM_GPU_TURBO=0` in `env.txt` turns it off.

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| AYN Odin 2 | Adreno 740 | Turnip v26.0.0_R8 by K11MCH1 (`--vulkan-driver`) | 1.1.4 | `v0.5.0` | plays: the title, character creation, a new save, Spring Village, combat; 30 fps Handheld and Docked in Spring Village at the 30 fps setting |

## Options

- `--vulkan-driver` with a Turnip build: the profile's `TU_DEBUG=sysmem` speed-up only works on Turnip. All testing used
  the K11MCH1 Turnip build v26.0.0_R8.
- Pause menu, Game tab: Handheld (recommended) holds 30 fps at a lower resolution. Docked is the default and looks sharper, but
  runs at 21-25 fps.
- Battery: about 4.9 W while playing in Docked mode without `DM_GPU_TURBO`, about 5 hours on the Odin 2. With turbo the
  draw is higher (not measured).

## Quirks and known issues

- Docked mode is below the game's 30 fps target in most scenes (see Frame rate).
- A fresh install with no saved shaders rendered the title, character creation, the opening and the first house without
  glitches.
- The game's opening movie (`Mov001_F.bk2`) was not seen in testing.
