# The Legend of Zelda: Link's Awakening (01006BB00C6F0000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # The Legend of Zelda: Link's Awakening 1.1.0 (v131072) main: the physics step (main+0x15730, main thread) rebuilds each world's PhysX completion task in place
  # (a PxLightCpuTask at world+0xc8) and passes it to simulate. A worker runs it (main+0x10ccf80: fetchResults, world+0x1d8 done = 1, notify, unlock) and then calls its
  # release (main+0x10cd070), which calls the task's own destructor: vtable back to the PxLightCpuTask base, mTm = 0 (main+0x10cced0). At 30 fps the game steps twice
  # per frame (step, wait, step, wait); the unlock hands the world's mutex to the woken main thread, and when the worker is descheduled there the main thread's
  # next step rebuilds the task before the late destructor runs, which then wipes the new task. The next simulation's NpSceneCompletion::release reads its mTm = 0:
  # crash at main+0x10ccf28 (lr main+0x13c236c; seen on the iPhone and in 2 of 5 Mac runs, in every Mac run under CPU load). On the Switch the release follows the
  # unlock within nanoseconds. The release becomes a plain ret: the destructor only resets the vtable and mTm (no resources), the step rebuilds every field, and this
  # release is used by this one vtable (main+0x1c51308 slot 7) only
  '909e904af78ac1b8deefe97ab2ccdb51': [
    (0x10cd070, '080040f9', 'c0035fd6', 'physics completion task release (calls its own destructor): ldr x8,[x0] → ret (no longer destroyed; the step function rebuilds it in place every step)'),
    # The game targets 60 and, when a frame runs past one 16.7 ms vsync, sets present interval 2 from main+0x1144c18 (the value is the byte at the frame-pacing object +0x4e1, plus one,
    # written through +0x812c by main+0x17838). On the tablet that byte flips after a frame of about 18 ms: one play had 196 such flips, and those windows' frame-time p95 became 32 ms
    # while the GPU stayed at 8-14 ms. The median frame was already 16.6 ms. These two calls are the ones the 1.1.0 stable-60fps exefs patch removes (its pchtxt offsets 0x10196d8 and
    # 0x10196f8). Both are bl on this main: main+0x10196d8 calls main+0x10411a0, main+0x10196f8 calls main+0x1019f20 (that one stores an aligned word at the pacing object +0x8128, beside
    # the interval). NOP leaves the flag store at main+0x10196e0 and the call at main+0x10196ec in place. A frame that is still late then stays a little over 16.7 ms instead of being held
    # for 33 ms. The 100 ms stalls while a new area is read are not this path.
    # Left commented out by default so older/slower chips can still fall back cleanly to 30 fps (present interval 2):
    # forcing interval 1 on devices that cannot hold 60 fps causes 16.7/33.3 ms frame-time judder and slow-motion
    # gameplay (since the game keeps stepping at 1/60s per frame without the 2-vsync step). Uncomment both lines
    # below and re-port if you want a steadier 60 fps on flagship chips.
    # (0x10196d8, 'b29e0094', '1f2003d5', 'dynamic drop to 30 fps: bl main+0x10411a0 → nop (the stable-60fps patch; present interval stays 1)'),
    # (0x10196f8, '0a020094', '1f2003d5', 'dynamic drop to 30 fps: bl main+0x1019f20 → nop (the stable-60fps patch; the call writes the pacing object +0x8128)'),
    # The constructor at main+0x1bdb0 reads a uint16 table index from [x19, #0xa0] (copied from [x21, #0x38]) and indexes the 0x275-entry table at main+0x1bcc898
    # (stride 0x58, reading byte +0x49 into [x19, #0xac]) without bounds-checking. When MainCamera_{0}P is created at main+0xbdaa0, that index is initialized to
    # 0xffff (main+0xbdb68: mov w8, #0xffff; strh w8, [sp, #0x68]), so main+0x1c180 reads main+0x214c889 — ~4.2 MB past the end of main (0x1d1c000). On Switch or
    # when ASLR happens to place a readable mapping (e.g. scudo:secondary) there, it reads garbage and survives; when that address is unmapped or PROT_NONE (most
    # runs, and almost always in release builds due to different startup mmap layout), it SIGSEGVs at main+0x1c180. Compacting the 6-qword copy at main+0x1c13c
    # with ldp/stp frees space in-place to zero [x19, #0xac] and bounds-check against 0x275 (matching main+0x238f8).
    (0x01c13c,
     'a9de40f968820991090500f9a9da40f9090100f9aaee40f9a9e640f90a1500f9aae240f9abea40f909ad01a90a0900f968424179090b80528add00904a6122910829099b0825413968ae00b9',
     '68820991a92a5ba9092900a9a92a5ca9092901a9a92a5da9092902a97fae00b9684241791fd5097122010054090b80528add00904a6122910829099b0825413968ae00b91f2003d51f2003d5',
     'constructor main+0x1bdb0: bounds-check table index [x19, #0xa0] against 0x275 (fixes 0xffff OOB read SIGSEGV at main+0x1c180)'),
  ],
}
