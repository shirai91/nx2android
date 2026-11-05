# Picross S+ 1.0.1: RP6 validation

Title ID: `0100D7B01AE58000`, update `v65536`, SDK 16.2.0. Tested with the
v0.3.0 runtime on Retroid Pocket 6, Android 13, Adreno 740 and manually selected
PurpleVK-public git-62ac221a33. Menu, puzzle rendering and controls work; the
tester completed a puzzle without issues. Full playthrough and all DLC are unverified.

## Startup fix

The generated profile exhausted its 4.38 GiB alias region: eight
`AllocateAddressRegion` failures (`0x1803`) preceded a SIGSEGV in `h_nvnBufferMap`.
The existing porter's `alias_headroom(profile, 8)` operation expands `mem_alias`
from `0x118000000` to `0x318000000`, and `mem_reserve` from `0x140000000` to
`0x340000000`. `app_pool` stays `0xCD500000`: this reserves virtual addresses,
not another 8 GiB of physical RAM. The test reached address extents beyond the
old limit and booted without those allocation failures. A 142-second capture
had no allocation failures or crash reports; subsequent manual puzzle play passed.
No NVN implementation or game binary patch is included.

## Limits

Pixel 9 Pro with system Mali-G715 also boots with the expanded profile, but has
persistent washed-out rendering and lines/triangles on updates. A separate
`DM_VK_NO_ZEROCOPY=1` diagnostic test removed the visible glitches according to
the tester, but was far too slow. This does not establish a precise root cause;
that option is excluded from the profile. Pixel 9a has not been tested.
Stock-driver RP6 behavior is not established. This contribution claims RP6 /
PurpleVK support only, not general Mali support.

## Review checklist

- [x] RP6 menu and puzzle manually verified
- [x] Address exhaustion reproduced and controlled headroom test passed
- [ ] Add a real RP6 gameplay screenshot and README games-table cell
- [ ] Retest this profile on current main (device test used v0.3.0 runtime)

No APKs, game data, drivers, keys, private FPS menu or Pixel diagnostics are included.
