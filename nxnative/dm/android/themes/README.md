# On-screen gamepad themes

Each folder here is a theme pack: the gamepad's buttons styled after one game, from the "Themed Virtual Controllers" design canvas. Only the buttons are
taken from a design, not its backdrop: the game is the background.

A port puts exactly one pack in the APK (`porter/pipeline.py` `theme`, packed by `mkapk.py --asset` as `assets/overlay/`): the game's own pack from
[themes.tsv](themes.tsv), the table of title IDs to packs, or `ergo` (the plain ergonomic layout) for a game the table does not name. The app reads it at
its first frame (`dm/android/overlay_theme.c`) and draws the gamepad from it (`gfx_vk_present.c` `ov_theme_draw`). An APK with no pack draws the gamepad
the old way, as outlines.

## A pack

| File | What |
|---|---|
| `tablet.dc.html`, `phone.dc.html` | The design's two artboards (1216x760 and 915x412 CSS px), copied from the canvas: the source the rest is made from |
| `tablet.png`, `phone.png` | The atlas of each layout: every sprite, 2x (tablet) or 3x (phone) the design's pixels, 256 colours with their alphas |
| `theme.tsv` | The controls' opacity, then per layout: the design's size, its atlas, its scale, how far a stick's knob travels; and every sprite: its control, d-pad direction, state (`idle`, `held`, `knob`, `decor`), place in the atlas and offset from the control's centre in CSS px |

Every pack shares one layout per form factor, the ergonomic one ([../overlay_default.tsv](../overlay_default.tsv)); a pack only changes how the
controls look. The app takes the phone layout when its window is at least 1.9 times as wide as it is high (a 20:9 phone), the tablet one otherwise, and
places each sprite at its offset from its control's centre, scaled by the window's height over the design's. The player can move and resize any control in the pause menu's layout editor (Gamepad tab, Edit layout) and set the controls' opacity there (20 to 80%); the theme's sprites follow, at each control's own size.

## Making or changing a theme

```bash
python devtools/theme_render.py --import <the canvas's project folder> [--only <pack>]
```

`--import` copies the canvas's artboards (`<pack>-tablet.dc.html`, `<pack>-phone.dc.html`; `Main` and `Phone` are `monstertrain`) into the packs, then
every pack (or `--only` one) is rendered with a headless Chrome or Edge (`--browser` to name one) and its atlases and `theme.tsv` written again.
`--layout` also writes `overlay_default.tsv` from the ergo design, when the layout itself changes. `--preview <pack> 2560x1600 out.png [--held a,zl,up]`
draws a pack as the app places it in a window of that size. A new game's pack also needs its row in `themes.tsv`; `tests/test_porter.py` checks every pack
and the table.

An artboard is read as the canvas writes it: the controls are the `<button>`s inside the `<!-- Controls` block, named by their `aria-label`; anything else
there except the input readout and the captions is a decoration, drawn with the control nearest to it; the `{{...}}` holes are filled per state from the
artboard's `renderVals` (a held button's `g`, the accent colour, the opacity).
