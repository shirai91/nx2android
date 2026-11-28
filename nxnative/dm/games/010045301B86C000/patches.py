# Romancing SaGa 2: Revenge of the Seven (010045301B86C000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a
# module's NSO build ID; CUTS as described there ("Registered patches")
CUTS = {
  # Romancing SaGa 2: Revenge of the Seven 1.1.0 (v196608) main: Unreal's texture streamer budget (main+0x3772030) keeps the mips a texture has loaded but no longer wants, as long as the pool
  # has room, one mip per pass through FStreamingRenderAsset::KeepOneMip (main+0x3794a64). That returns the size the extra mip adds, and the keep loop (main+0x37728a0) takes 0 as "nothing
  # left to keep" and drops the texture from the list. The texture's sizes per mip count come from nvnTextureBuilderGetStorageSize, and the streamed textures are sparse, so their
  # storage is whole 64 KB tiles: 1 to 8 mips (up to 128×128) all take 64 KB, and going from the 7 mips the minimum leaves to 8 adds nothing. Whenever a texture is not drawn for a frame
  # (the start menu covering the field, a battle or dungeon camera cut), it wants its 7 minimum mips, keeps one more and stops there, with the pool far from full, and its 4 big mips
  # are streamed out and read back half a second later: 60 MB at every menu toggle on the Odin 2, the field and the characters blurred until then. KeepOneMip returns at least
  # 1 byte for a mip it did keep, so the loop goes on to the mips that do add size; its two "nothing to keep" exits go to the identical "return 0" just before it (main+0x3794a5c)
  'd7a32c718e9d63639fa1cc7ef29b836d': [
    (0x3794a68, 'c80100b4', 'a8ffffb4', 'KeepOneMip: cbz x8 (no texture) → the return 0 at 0x3794a5c'),
    (0x3794a84, 'ea000054', 'cafeff54', 'KeepOneMip: b.ge (at the resident or allowed mips) → the return 0 at 0x3794a5c'),
    (0x3794a9c, 'c0035fd6', '1f0400f1', 'KeepOneMip: ret → cmp x0, #1'),
    (0x3794aa0, 'e0031faa', '00a49f9a', 'KeepOneMip: mov x0, xzr → csinc x0, x0, xzr, ge (a mip kept adds at least 1 byte); the ret after it returns'),
  ],
}
