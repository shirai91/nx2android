# 三國志14 with 威力加強版 (`0100CD4012DCA000`)

Update v786432 (1.1.1) over the base game, with add-on content 49 (the expansion pack itself). Koei Tecmo's KTGL on NVN, SDK 10.4.1. Modules `main`, `subsdk0` (NintendoSDK_movie: the MP4 opening and event movies) and `sdk`. Plays on Android at 30 fps, as on the Switch.

## What it needs

| Setting | Where | Why |
|---|---|---|
| the DSP service's work-buffer budget, with the local CPU/manual buffer supplied by the host | `dm/host/hle_audren.c` (`ar_service_work_size`, `ar_open`) | KTGL's internal pools have a few hundred bytes to spare; the CPU-sized allocation (1.37 MB more) pushed one over and the subsystem's process step returned -51; see [Audio](#audio) |
| `nn::fs::QueryMountRomCacheSize(size_t*, int)` and `nn::fs::MountRom(const char*, int, void*, size_t)` | `dm/host/hle_fs.c` | the game calls the option-word forms at `main+0x5a20` / `main+0x5a80`; without them it aborts at `[RED] sf: the game connects to service fsp-srv itself` |

No game-specific code cuts. The frame rate needs none: the game is 30 fps on the Switch too.

## Audio

`OpenAudioRenderer` is taken over (CPU rendering + manual execution, then the host's render thread), and the game sizes its own allocation by `GetAudioRendererWorkBufferSize`. Answering with the local SDK's CPU + manual size (0x60e000) rather than the DSP service's requirement (0x4c4000) made the game reserve 1.37 MB more than on hardware. Its internal pools are laid out to within a few hundred bytes, so a later allocation failed, the subsystem's process step returned -51, audio was torn down and the boot exited cleanly (`nnMain returned 0`) with no error line. The service's own formula now answers and the CPU representation is host storage taken at open and freed at close.

The same game's Eden report ([Issue-Reports#387](https://github.com/eden-emulator/Issue-Reports/issues/387)) found the same failure on an emulator and fixed it by rebalancing the pools themselves — same 0x7248-byte request refused with 0x6CD0 left.

## Memory

The defaults hold. The game makes no `nn::os::AllocateAddressRegion` region in anything tested here: no `[mem] ... address regions live` line appears,
and the campaign runs the same with `mem_alias` at 4.375 GB and at the profile's 256 MB. The SDK's node area is cut from 1 GB to 62 MB by
`dm/tools/sdk_patches.py` for this sdk build (`d9add76fc264`), which is where that cut belongs; the profile does not carry it.

The first turn of a campaign takes a while — a few seconds of the screen sitting still before the map comes up. It is the engine loading the scenario,
not a freeze: the frame count keeps rising and the sound keeps playing, and the turn itself then runs at the same rate as the rest of the map.

## Files and shaders

- RomFS: 12 GB, 1,019 files in the data pack. `0002/` holds the engine's `.rdb` archives and their `.bin` volumes, `0001_SC` / `0001_TC` the message data, `0004/` the maps; add-on content 49 is the expansion's own volume.
- `shader_split.py` finds 55,383 shaders; the Vulkan bundle carries 55,391 (the split's supplemental vertex/pixel pairs included). All translate. GLSLC outputs in `main` and the G2S containers in `0002/` are the two sources, as in the other KTGL games.
- The game reads its archives by offset, so the `.bin` extensions stay uncompressed (`zip_store`).

## Frame rate

30 fps, as on the Switch: frame time p50 33.3 ms, GPU 20.7 ms and replay 2.6 ms a frame on the campaign map, no frame dropped on screen.

60 fps was investigated and is out of reach on this GPU. The four places the engine's cap lives are known and each can be moved:

| What | Where | How |
|---|---|---|
| the frame-rate table | `main+0x1e47120`, entries 30 and 60 | 68 reads pick an entry through the u32 at `main+0x27c16e0` (`ldr s0,[x9,x8,lsl #2]`), zero at boot and never written |
| the lock's own copy | `main+0x896c70`: `ldr w8,[x22,#0x30]` → `[x23+0xc0]` | reads a runtime field that holds 30 whatever the table says |
| the clock step | `main+0x1e3a484`, the float `main+0x11a0020` adds | 1/30 at boot; must move with the table or the game runs at half speed |
| the present interval | `main+0xa677f4` sets 2 | the host can hold it at 1 |

FPSLocker's config for this build (`33E595CFF4FCB981.yaml`) moves the first three; the fourth is the host's `present_interval display`. With all four applied the lock is lifted (the game asks for 60 and the host presents one frame a tick) and the frame still takes 33.3 ms: the GPU needs 20.7 ms for the map against 7 ms on the Mac host for the same scene, so the ceiling on an Adreno 830 is about 48 fps. `DM_RENDER_SCALE=0.75` and handheld mode (`DM_OPMODE=0`) bring the GPU time to 4–14 ms but the frame stays at 33 ms, so the engine's own step — not the GPU — sets the pace once the lock moves; that is where a 60 fps attempt has to continue.

## Testing

Xiaomi 2410DPN6CC (haotian, Snapdragon 8 Elite, Adreno 830, Android 16), wireless adb, `porter port --experimental --touch`:

- the logos, the title screen, the settings screen (every row drawn and switchable), a new game from the faction select through the opening narration into the campaign map, a city panel opened on the map, the officer list, and a save the game commits (#7) without an error;
- 30 fps with frame time p50 33.3 ms, p95 35 ms, 0 frames dropped on screen; audio 0 underruns over 48,000 frames with 5 catch-up renders;
- a re-run after a restart continues from the save.

## Open

- **Touch on list screens.** Touch reaches the game correctly (Start/End balanced, coordinates across the 1280×720 panel, gestures recognized). The officer list still behaves as on hardware: a tap selects the officer and the game moves focus into the detail pane, and list paging is a right-stick operation. The Switch's own touch cannot do the scrollbar or some list operations either (KOEI's FAQ). A two-finger swipe as synthetic buttons, as NX2Apple's `DM_SWIPE_BTNS` does for the same game, is the planned fix; it is not implemented here.
- **60 fps**: see above; the engine's step after the lock moves is the next thing to find.
