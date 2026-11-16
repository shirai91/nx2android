# Ultra Kaiju Monster Rancher (01008E0019388000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS as described there ("Registered patches").
CUTS = {
  # Ultra Kaiju Monster Rancher v0 (1.0.1) main
  '53384cc3d2b4ca9f166e3afb7f7b9e21': [
    # The game's network start-up (main+0x16b3f4, from the init function main+0x106c) makes an SSL context (main+0x1bf13c: nn::ssl::Context::Create(SslVersion Auto), a failure returns -1) and gives up the whole start-up when it
    # fails. The host cuts every ssl call that reaches the service to RES_FAIL when offline (hle_net.c), so the first nnMain run ended after the window and the shader bundle were up: nnMain's init returned false, the
    # teardown (main+0x1034) ran on singletons that were never made and wrote through a null pointer: CRASH sig=11 addr=0x463410 pc=main+0x194f34 lr=main+0x1058 (the game's destructor of the global at *(main+0xadff48)).
    # The context is only used for online play, which stays out of scope (every Connection call still fails): only this one call is replaced, by "success", so the game starts offline. Other games keep the failing ssl cut.
    (0x1bf16c, '39bc0f94', 'e0031f2a', 'nn::ssl::Context::Create result: bl Context::Create → mov w0, wzr (the context is used only for online play)'),
  ],
}
