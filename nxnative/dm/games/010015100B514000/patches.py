# Super Mario Bros. Wonder (010015100B514000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # Super Mario Bros. Wonder v0 main
  'cd6e42aee7934f4d8393cd43893ae25e': [
    # Resource heap slack: each resource unit's ExpHeap is created with the resource size table value + 0x1008 (main+0x1e603c → ExpHeap::create main+0x4aac38); the table holds Nintendo's measured peaks with almost no margin
    # (PlayerYoshiAnm uses 99%). On the host, Model/NpcCommonerA.bfres in the opening course Course900 is about 0x70 bytes short: after the model resource's last GPU chunk (MatBlock,
    # 0x9000 bytes, 4 KB aligned, main+0x4290c0) is placed, its 0x598-byte descriptor does not fit, the GPU allocation fails → a buffer is built on a null memory pool → null read crash at main+0x106f5c8.
    # Where this differs from hardware is not known (every allocation size before this block is determined by game code and data); for now every resource heap gets +16 KB.
    (0x1e62fc, 'e00317aa', 'e0124091', 'resource heap size: mov x0,x23 → add x0,x23,#0x4000 (ExpHeap::create size argument +16 KB)'),
  ],
  # Super Mario Bros. Wonder 1.2.1 (v327680) main: same as above. Location carried over from v0 by dm/tools/nso_fnmap.py (containing function 0x1e603c → 0x1cf6f0 identical after address masking, ExpHeap::create 0x4aac38 → 0x4c4a18)
  'ff773e90972d544eb79406eaa65396d5': [
    (0x1cf9b0, 'e00317aa', 'e0124091', 'resource heap size: mov x0,x23 → add x0,x23,#0x4000 (ExpHeap::create size argument +16 KB)'),
  ],
}
