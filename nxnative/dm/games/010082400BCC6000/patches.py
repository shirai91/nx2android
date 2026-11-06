# Untitled Goose Game (010082400BCC6000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # Untitled Goose Game 1.1.2 (v458752) main, Unity 2018.4's NVN texture binding (main+0x1b2a0c0, on the render thread): a first pass over a draw's bindings puts each texture that is ready
  # (an NVN texture ID at +0xd0 and a sampler ID at +0xd4) in a local array of 32 slots and skips the others; then the bindings that take their sampler from another slot (flag bit 10) read
  # that slot's texture from the array for its sampler ID (main+0x1b2a414). The array is not cleared, so a slot the first pass skipped holds whatever was on the stack. On the Switch that
  # is an older texture of the same call; here the host's functions leave other values there, and a null one crashed the render thread at its sampler ID (main+0x1b2a424, address 0xd4) in
  # about 1 launch in 5, at the load after the title's second garbage collection. The deferred binding now skips an empty slot, as the first pass skips a texture that is not ready (the
  # slot keeps the handle it had). Room for the check: the device pointer and the handle function are loaded from their GOT entries through one adrp (0x3fb3e80 and 0x3fb4538)
  'dbc3424ab22de644a987386dc692d327': [
    (0x1b2a40c, '4a2401b0', '497b69f8', 'deferred sampler slot: adrp x10 → ldr x9,[x26,x9,lsl #3] (the slot\'s texture, moved up)'),
    (0x1b2a410, '4a4147f9', 'a90300b4', 'deferred sampler slot: ldr x10,[x10,#0xe80] → cbz x9,0x1b2a484 (an empty slot: next binding)'),
    (0x1b2a414, '497b69f8', '01d140b9', 'deferred sampler slot: ldr x9,[x26,x9,lsl #3] → ldr w1,[x8,#0xd0] (texture ID)'),
    (0x1b2a418, '01d140b9', '22d540b9', 'deferred sampler slot: ldr w1,[x8,#0xd0] → ldr w2,[x9,#0xd4] (sampler ID)'),
    (0x1b2a41c, '482401d0', '482401b0', 'deferred sampler slot: adrp x8,0x3fb4000 → adrp x8,0x3fb3000 (both GOT entries from one page)'),
    (0x1b2a420, '400140f9', '004147f9', 'deferred sampler slot: ldr x0,[x10] → ldr x0,[x8,#0xe80] (the device pointer\'s GOT entry)'),
    (0x1b2a424, '22d540b9', '089d4af9', 'deferred sampler slot: ldr w2,[x9,#0xd4] → ldr x8,[x8,#0x1538] (the handle function\'s GOT entry)'),
    (0x1b2a428, '089d42f9', '000040f9', 'deferred sampler slot: ldr x8,[x8,#0x538] → ldr x0,[x0] (the device)'),
  ],
}
