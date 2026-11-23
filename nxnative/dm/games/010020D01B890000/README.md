<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->
# Apollo Justice: Ace Attorney Trilogy

![Apollo Justice: Ace Attorney Trilogy](../../../../docs/images/apollo-justice-ace-attorney-trilogy.jpg)

| | |
|---|---|
| Status | **wip** |
| Title ID | `010020D01B890000` |
| Tested with nx2android | `v0.4.0` |
| Tested build and device | v0 OnePlus 8 IN2010 (Snapdragon 865, Adreno 650) |
| App package | `org.nx2a.aa456` |

Read the profile's porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list](../../../../README.md#games).
<!-- end of the game facts -->

## Game version

- Base game 1.0.1 (`010020D01B890000`). No update was tested, and no later update is known to work or to fail.

## Frame rate

- 30 fps, as on the Switch: frame time p50 33.2 ms in the Court Record's 3D exhibits on the OnePlus 8 (the window presents at
  60 Hz, so half the vsyncs carry no new frame).

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| OnePlus 8 (IN2010) | Adreno 650 | Turnip, KIMCHI v26.0.0 R8, packed with `--vulkan-driver` | base game 1.0.1 | `v0.4.0` | plays: the launcher, a loaded save, the Court Record's Attorney's Badge (rotated to its back) and Deadly Bottle (its label), through repeated switches between the two exhibits |

## Options

- `--vulkan-driver` with a Turnip build. The OnePlus 8's own driver reports Vulkan 1.1, which this backend cannot use; with it the
  app stops at "[vk] the device has neither dynamic rendering nor imageless framebuffers with render pass 2".
- No `--firmware`, `--shared-font`, `--language` or in-game setting was needed for what was tested.

## Quirks and known issues

- **Work in progress**: only the launcher, one loaded save and the two 3D exhibits above were checked on a device. Later episodes,
  the pause menu's save export/import, the rest of the trilogy, and other GPUs (Mali, Xclipse) are unchecked.
- The two exhibits are the parts that exercise sparse buffer textures; a regression there shows as a missing label or a plain
  gold badge back rather than as a crash.
