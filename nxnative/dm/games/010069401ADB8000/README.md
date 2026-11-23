<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->
# Unicorn Overlord

![Unicorn Overlord](../../../../docs/images/unicorn-overlord.jpg)

| | |
|---|---|
| Status | **boots** |
| Title ID | `010069401ADB8000` |
| Tested with nx2android | `v0.1.3` |
| Tested build and device | v262144 Lenovo Legion Y700 TB322FC (Snapdragon 8 Elite, Adreno 830, Android 16) |
| App package | `org.nx2a.unicornoverlord` |
| Needs | a GPU that reads BC-compressed textures (Adreno, Xclipse; not Mali or PowerVR) |

The profile sets these for you (`android.tsv`); a player changes the pause menu ones in the game, and the env rows in the data pack's `env.txt`:

| Row | What it does |
|---|---|
| `docked=0` | pause menu default on first launch: docked mode (1) or handheld mode (0) |
| `DM_NVN_ASYNC=1` | asynchronous replay: submissions and presents run on a replay thread, syncs wait for the job's replay and then its GPU fence |
| `DM_VK_UBWC=1` | framebuffer compression mode bits (Adreno UBWC) |
| `DM_ADAPT_UP_PCT=50` | the governor steps up a rate when frames fit in this percent of the faster rate's budget (1..99) |
| `DM_VK_PASS_SPLIT=512` | split a pass into command buffers every N draws; 0 = never |
| `DM_PIN_REPLAY=prime` | pin the replay thread to fast cores: 0 = no, "prime" = the fastest core, "need" = the fastest core only while the replay is slow, other values = the big cores |
| `DM_VK_SETSPLIT=1` | 1 = a descriptor set change may split the command buffer |

Read the profile's porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list](../../../../README.md#games).
<!-- end of the game facts -->

## Game version

- Update 1.05.
- The profile was checked with `v262144` (the `tested` row of android.tsv).

## Frame rate

- 60 fps.

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| Lenovo Legion Y700 TB322FC (Snapdragon 8 Elite, Adreno 830, Android 16) | Adreno 830 | not recorded | 1.05 (v262144) | `v0.1.3` | boots |

## Options

- Not recorded.

## Quirks and known issues

- **boots**: saving and a full battle not tested yet.
