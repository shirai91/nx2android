<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->
# The Great Ace Attorney Chronicles

![The Great Ace Attorney Chronicles](../../../../docs/images/the-great-ace-attorney-chronicles-chinese.jpg)

| | |
|---|---|
| Status | **wip** |
| Title ID | `0100D7F00FAAA000` |
| Tested with nx2android | `v0.4.0` |
| Tested build and device | v0 OPPO Pad OPD2101 (Snapdragon 870, Adreno 650, Android 15) |
| App package | `org.nx2a.t0100d7f00faaa000` |
| Vulkan driver | Turnip: the port packs Mesa's Turnip driver when the porter finds it (`vulkan_driver turnip`) |

Read the profile's porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list](../../../../README.md#games).
<!-- end of the game facts -->

## Game version

- Base game 1.0.0, the Chinese release (`1.0.0(CN1.3)`, `build v0`). No update has been tried: this profile carries a build row for the base game only.

## Frame rate

- 30 fps by default, as on the Switch, and steady: the frame time's p50 is 33.3 ms and the GPU takes 10-14 ms of it, so the engine's own 1/30 step is the limit, not the GPU.
- 60 fps on request: the profile carries one frame-lock row ([`fps_locks.tsv`](fps_locks.tsv)) that raises MT Framework's render ceiling `sApp.mMaxFps` from 30 to 60 and leaves the logical step `mFps` at 30. The frame loop divides one by the other on every frame, so `mDeltaTime` comes out at half the real frame time and the 83 readers of `mDeltaTime` stay 1:1 - typing, Auto-Play, transitions, camera moves and 3D animation all keep their speed. Raising `mFps` as well makes the ratio 1.0 and the whole game runs at double speed.
  - Reached with a frame-rate target (`DM_FPS_TARGET=60`), or from the pause menu's 60 fps switch where the host has one.
  - Measured with the row, 60 second windows:
    - a Snapdragon 8 Elite class phone (Adreno 830), docked at 1080p inside the first episode: frame p50 16.67 / p95 18.20 / p99 19.19 ms, GPU 3.0 ms, 1 dropped frame of 7 800
    - the OPPO Pad (Adreno 650, Turnip): 60 at the handheld 720p picture, about 50 at the docked 1080p one (frame p50 19.9 ms, GPU 17.1 ms)
    - a Dimensity 900 (Mali-G68): **60 at the handheld 720p picture** - frame p50 16.66 / p95 17.54 / p99 19.12 ms, GPU 14.1 ms, 16 dropped frames of 15 300 over 250 s - and about 40 at the docked 1080p one (frame p50 24.9 ms, GPU 22.9 ms)
  - That split is the resolution, not the chip: the docked 1080p picture is 2.25x the pixels of the handheld 720p one. A phone that misses 60 docked can hold it handheld.

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| OPPO Pad (OPD2101) | Adreno 650 | Turnip 25.99.99 (bundled) | 1.0.0 (CN1.3) | `v0.4.0` | plays: the title screen and the first episode's office with its dialogue, at 30 fps; both pause-menu tabs, the layout editor, touch in handheld and docked, save export and import |

## Options

- `--vulkan-driver` on Adreno devices whose own driver is Vulkan 1.1: the OPPO Pad's reports `Vulkan 1.1.128` and has neither dynamic rendering nor imageless framebuffers, and the host picks a Vulkan 1.3 device
- No setting inside the game had to be changed for the parts that were tested

## Quirks and known issues

- Without `tex_flush_upload` and `tex_dirty_upload` (this profile sets both) every 3D prop renders black: the game copies each texture into a CPU-cached pool after `nvnTextureInitialize` and never flushes it, and `main` has no cache maintenance at all
- No cap removal without the frame-lock row: the gate is inside the engine (`sApp.mMaxFps`), so a faster GPU alone does not raise it; with the row the ceiling is 60 and the speed stays 1:1 (see [Frame rate](#frame-rate))
- Not played end to end: the title screen, the first episode's office and the pause menu are what was checked
