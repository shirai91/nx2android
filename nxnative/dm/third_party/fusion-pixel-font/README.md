# Fusion Pixel Font

The Android overlay's text (the pause menu, the text entry, the layout editor, the frame rate counter): [TakWolf/fusion-pixel-font](https://github.com/TakWolf/fusion-pixel-font) release 2026.10.25, 12 px proportional, zh_TW. It joins Ark Pixel with Cubic 11 and Galmuri, which fill Ark's gaps in Traditional Chinese and Hangul.

`fusion-pixel-12px-proportional-zh_tw.nxpf` is `fusion-pixel-12px-proportional-zh_tw.bdf` from the release's `fusion-pixel-font-12px-proportional-bdf-v2026.10.25.zip`, converted without changing a glyph:

```
python nxnative/dm/tools/make_pixel_font.py fusion-pixel-12px-proportional-zh_tw.bdf fusion-pixel-12px-proportional-zh_tw.nxpf
```

The fonts are under the SIL Open Font License 1.1 (`OFL.txt`, and `LICENSES/` for the fonts it joins); none declares a Reserved Font Name. Every APK carries these license files beside the font, in `assets/font/`.
