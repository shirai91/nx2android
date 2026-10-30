# The on-screen gamepad's themes: each design (a "Themed Virtual Controllers" canvas artboard, .dc.html) rendered once into a sprite atlas and its table, which
# porter packs into a game's APK (nxnative/dm/android/themes/README.md). The designs are kept beside what they render, so a theme is made again when its design changes.
#   theme_render.py [--import <canvas project folder>] [--only <pack>] [--browser <chrome or msedge>] [--layout]
#   theme_render.py --preview <pack> <W>x<H> <out.png> [--held a,zl,up]   (the pack as the app places it in a window of that size)
# --import copies the canvas's artboards (<pack>-tablet.dc.html / <pack>-phone.dc.html, Main / Phone = monstertrain) into themes/<pack>/ first.
# --layout writes the gamepad's layouts (dm/android/overlay_default.tsv) from the ergo pack's design: every theme shares its controls' places and sizes.
# Each pack gets <layout>.png (the atlas) and theme.tsv. Only the controls are rendered (the backdrop, the input readout and the captions are the design's
# mock-up, not the gamepad), in each state the app draws: idle and held for a button, the stick's base idle and held, its knob, the menu
# button, and the decorations between controls (each drawn with the control nearest to it). A headless Chrome or Edge renders them, 2x (tablet) or 3x (phone)
# the design's CSS pixels; Pillow cuts each sprite to its pixels and packs the atlas.
import argparse, json, math, os, re, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
THEMES = ROOT / 'nxnative/dm/android/themes'
LAYOUT_TSV = ROOT / 'nxnative/dm/android/overlay_default.tsv'
LAYOUTS = {'tablet': 2, 'phone': 3}   # the design's form factors and the scale each is rendered at (a 2560x1600 tablet is 2.1x its 1216x760 design, a 2400x1080 phone 2.6x its 915x412)
MARGIN = 14   # CSS px around a control: its shadows and the 3 px a held button moves down
BROWSERS = [os.path.expandvars(p) for p in (r'%ProgramFiles%\Google\Chrome\Application\chrome.exe', r'%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe',
            '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome', '/usr/bin/google-chrome', '/usr/bin/chromium')]

# the design's buttons (aria-label) → the layout's control and, for the d-pad, its direction
CONTROLS = {'Emulator settings': ('menu', '-'), 'Minus button': ('minus', '-'), 'Plus button': ('plus', '-'), 'ZL trigger': ('zl', '-'), 'L button': ('l', '-'),
            'ZR trigger': ('zr', '-'), 'R button': ('r', '-'), 'Left stick': ('stick_l', '-'), 'Right stick': ('stick_r', '-'), 'D-pad up': ('dpad', 'up'),
            'D-pad down': ('dpad', 'down'), 'D-pad left': ('dpad', 'left'), 'D-pad right': ('dpad', 'right'), 'L3, left stick click': ('ls', '-'),
            'R3, right stick click': ('rs', '-'), 'A button': ('a', '-'), 'B button': ('b', '-'), 'X button': ('x', '-'), 'Y button': ('y', '-')}
HELD_ID = {'minus': 'MINUS', 'plus': 'PLUS', 'zl': 'ZL', 'l': 'L', 'zr': 'ZR', 'r': 'R', 'ls': 'L3', 'rs': 'R3', 'a': 'A', 'b': 'B', 'x': 'X', 'y': 'Y',
           'up': 'UP', 'down': 'DOWN', 'left': 'LEFT', 'right': 'RIGHT'}   # its id in the design's renderVals (p.<ID>.g)
# the layout table's rows, in drawing order: kind, DM_BTN_* bit, the fallback's label (overlay_layout.h draws it when the app has no theme)
TABLE = [('stick_l', 'stick_l', '-', '~'), ('stick_r', 'stick_r', '-', '~'), ('dpad', 'dpad', '-', '~'), ('a', 'button', 'DM_BTN_A', 'A'), ('b', 'button', 'DM_BTN_B', 'B'),
         ('x', 'button', 'DM_BTN_X', 'X'), ('y', 'button', 'DM_BTN_Y', 'Y'), ('l', 'button', 'DM_BTN_L', 'L'), ('zl', 'button', 'DM_BTN_ZL', 'ZL'),
         ('r', 'button', 'DM_BTN_R', 'R'), ('zr', 'button', 'DM_BTN_ZR', 'ZR'), ('minus', 'button', 'DM_BTN_MINUS', '-'), ('plus', 'button', 'DM_BTN_PLUS', '+'),
         ('ls', 'button', 'DM_BTN_LS', 'L3'), ('rs', 'button', 'DM_BTN_RS', 'R3'), ('menu', 'menu', '-', '~')]
IMPORT = {'Main': ('monstertrain', 'tablet'), 'Phone': ('monstertrain', 'phone')}
TAG = re.compile(r'<!--.*?-->|<(/?)([a-zA-Z][\w-]*)([^>]*?)(/?)>', re.S)


def element_end(t, start):   # the end of the element whose start tag is at t[start]
    depth = 0
    for m in TAG.finditer(t, start):
        if m.group(0).startswith('<!--'): continue
        if m.group(4): depth += 0
        elif m.group(1): depth -= 1
        else: depth += 1
        if depth == 0: return m.end()
    raise ValueError(f'unbalanced element at {start}')


def children(t, start):   # the top-level children (start, end) of the element at t[start]
    out, i = [], t.index('>', start) + 1
    for m in TAG.finditer(t, i):
        if m.start() < i or m.group(0).startswith('<!--'): continue
        if m.group(1): return out   # the parent's end tag
        e = m.end() if m.group(4) else element_end(t, m.start())
        out.append((m.start(), e)); i = e
    raise ValueError('unbalanced')


def style_of(el):
    m = re.search(r'\sstyle="([^"]*)"', el[:el.index('>') + 1]); return m.group(1) if m else ''


def px(style, key, el=''):   # a length in px from the style (or the element's attribute: an <svg>'s width)
    m = re.search(rf'(?:^|;)\s*{key}:\s*(-?[\d.]+)px', style) or re.search(rf'\s{key}="([\d.]+)"', el[:el.index('>') + 1] if el else '')
    return float(m.group(1)) if m else None


def add_style(el, css):   # css added to the element's own style
    end = el.index('>')
    if re.search(r'\sstyle="', el[:end]): return re.sub(r'\sstyle="', f' style="{css} ', el, count=1)
    return el[:end] + f' style="{css}"' + el[end:]


class Design:
    """One artboard: its controls, decorations and the values its renderVals gives each state"""
    def __init__(self, path):
        t = path.read_text(encoding='utf-8'); self.path = path
        props = json.loads(re.search(r"data-props='([^']*)'", t).group(1))
        self.W, self.H = props['$preview']['width'], props['$preview']['height']
        self.accent = props['accent']['default']; self.alpha = props.get('opacity', {}).get('default', 1)
        self.g = re.search(r'g: on \? ([\d.]+)', t).group(1); self.glow = re.search(r'glow: on \? (\d+)', t).group(1)
        self.travel = float(re.search(r'this\.stickTravel = ([\d.]+)', t).group(1))
        helmet = re.search(r'<helmet>(.*?)</helmet>', t, re.S).group(1)
        self.links = ''.join(re.findall(r'<link [^>]*>', helmet))
        self.css = re.sub(r'body\{[^}]*\}', '', re.search(r'<style>(.*?)</style>', helmet, re.S).group(1))
        root = t.index('<div', t.index('</helmet>')); rs = style_of(t[root:])
        self.font = re.search(r'font-family: ([^;]+)', rs).group(1); self.color = re.search(r'(?:^|; )color: ([^;]+)', rs).group(1)
        ctl = t.index('<div', t.index('<!-- Controls'))
        self.buttons, self.decor = [], []   # (control, part, html, (x, y, w, h)); (html, bbox)
        for s, e in children(t, ctl):
            el = t[s:e]; st = style_of(el); tag = re.match(r'<([\w-]+)', el).group(1)
            box = (px(st, 'left'), px(st, 'top'), px(st, 'width', el), px(st, 'height', el))
            if tag == 'button':
                label = re.search(r'aria-label="([^"(]+?)\s*(?:\(|")', el).group(1)
                self.buttons.append((*CONTROLS[label], el, box))
            elif tag == 'sc-if' or '{{last}}' in el: continue   # the captions, the input readout: the design's mock-up
            else: self.decor.append((el, box))
        if None in (v for *_, b in self.buttons for v in b): raise ValueError(f'{path.name}: a control without its place')

    def fill(self, el, held=(), stick=False, menu=False):   # the template's holes for one state
        el = re.sub(r'\son[A-Z]\w*="\{\{[^}]*\}\}"', '', el)
        def var(m):
            k = m.group(1).split('.')
            if k == ['accent']: return self.accent
            if k[0] == 'p': on = k[1] in held; return {'g': self.g if on else '0', 'y': '3' if on else '0', 'o': '1' if on else '0'}[k[2]]
            if k[0] in ('ls', 'rs'): return {'x': '0', 'y': '0', 'r': '0', 'glow': self.glow if stick else '0', 'o': '0.8' if stick else '0'}[k[1]]
            if k[0] == 'menu': return {'x': '0', 'y': '0', 'o': '1' if menu else '0'}[k[1]]
            raise ValueError(f'{self.path.name}: unknown hole {m.group(0)}')
        return re.sub(r'\{\{([\w.]+)\}\}', var, el)

    def centres(self):   # each control's centre and size in CSS px: the d-pad's is its four keys' box
        out = {}
        for c, part, _, (x, y, w, h) in self.buttons:
            if c in out: x0, y0, x1, y1 = out[c]; out[c] = (min(x0, x), min(y0, y), max(x1, x + w), max(y1, y + h))
            else: out[c] = (x, y, x + w, y + h)
        return {c: ((x0 + x1) / 2, (y0 + y1) / 2, x1 - x0, y1 - y0) for c, (x0, y0, x1, y1) in out.items()}

    def sprites(self):   # [(control, part, state, html, bbox, margin)]
        out = []
        for c, part, el, box in self.buttons:
            if c in ('stick_l', 'stick_r'):
                k = re.search(r'<span [^>]*translate\(\{\{[lr]s\.x\}\}', el); ks, ke = k.start(), element_end(el, k.start())
                knob = add_style(el[ks:ke], 'visibility: visible;')
                base, alone = el[:ks] + add_style(el[ks:ke], 'visibility: hidden;') + el[ke:], add_style(el[:ks], 'visibility: hidden;') + knob + el[ke:]
                out += [(c, part, 'idle', self.fill(base), box, MARGIN), (c, part, 'held', self.fill(base, stick=True), box, MARGIN),   # (the held knob's glow came out
                        (c, part, 'knob', self.fill(alone), box, MARGIN)]                                                                  #  cut square by Chrome: the base's glow says held)
            elif c == 'menu': out += [(c, part, 'idle', self.fill(el), box, MARGIN), (c, part, 'held', self.fill(el, menu=True), box, MARGIN)]
            else:
                hid = HELD_ID[part if part != '-' else c]
                out += [(c, part, 'idle', self.fill(el), box, MARGIN), (c, part, 'held', self.fill(el, held=(hid,)), box, MARGIN)]
        cen = self.centres()
        for el, (x, y, w, h) in self.decor:   # drawn with the control nearest to it (the face buttons' ring, the d-pad's centre)
            near = min((c for c in cen if c != 'menu'), key=lambda c: math.hypot(cen[c][0] - x - w / 2, cen[c][1] - y - h / 2))
            out.append((near, '-', 'decor', self.fill(el), (x, y, w, h), MARGIN))
        return out


def browser(given):
    for b in ([given] if given else []) + BROWSERS + [shutil.which(n) for n in ('chrome', 'msedge', 'chromium', 'google-chrome')]:
        if b and Path(b).is_file(): return b
    sys.exit('[RED] no Chrome or Edge found: give --browser')


def render(d, scale, exe, tmp):
    """The sprites of one design, each cut to its pixels: [(control, part, state, PIL image, (ox, oy) of its top left from the control's centre in CSS px)]"""
    from PIL import Image
    sprites, cells, x, y, row, PW = d.sprites(), [], 0, 0, 0, 1600
    for c, part, state, el, (bx, by, bw, bh), m in sprites:   # a cell per sprite, in rows
        cw, ch = int(bw + 2 * m + 1), int(bh + 2 * m + 1)
        if x + cw > PW: x, y, row = 0, y + row + 4, 0
        cells.append((x, y, cw, ch)); x += cw + 4; row = max(row, ch)
    PH = y + row
    body = ''.join(f'<div style="position: absolute; left: {cx}px; top: {cy}px; width: {cw}px; height: {ch}px; overflow: hidden">'
                   f'<div style="position: absolute; left: {m - bx}px; top: {m - by}px; width: {d.W}px; height: {d.H}px">{el}</div></div>'
                   for (cx, cy, cw, ch), (_, _, _, el, (bx, by, _, _), m) in zip(cells, sprites))
    page = (f'<!doctype html><html lang="en"><head><meta charset="utf-8">{d.links}<style>{d.css}\nhtml,body{{margin:0;background:transparent}}</style></head><body>'
            f'<div style="position: relative; width: {PW}px; height: {PH}px; font-family: {d.font}; color: {d.color}">{body}</div></body></html>')
    html, png = tmp / 'page.html', tmp / 'shot.png'
    html.write_text(page, encoding='utf-8'); png.unlink(missing_ok=True)
    subprocess.run([exe, '--headless=new', '--disable-gpu', '--hide-scrollbars', f'--force-device-scale-factor={scale}', '--default-background-color=00000000',
                    f'--window-size={PW},{PH}', '--virtual-time-budget=15000', '--run-all-compositor-stages-before-draw', f'--user-data-dir={tmp / "profile"}',
                    f'--screenshot={png}', html.as_uri()], check=True, capture_output=True, timeout=180)
    shot = Image.open(png).convert('RGBA')
    if shot.size != (PW * scale, PH * scale): raise RuntimeError(f'{d.path.name}: the screenshot is {shot.size}, not {(PW * scale, PH * scale)}')
    cen, out = d.centres(), []
    for (cx, cy, cw, ch), (c, part, state, _, (bx, by, _, _), m) in zip(cells, sprites):
        im = shot.crop((cx * scale, cy * scale, (cx + cw) * scale, (cy + ch) * scale))
        bb = im.getchannel('A').point(lambda a: 255 if a > 3 else 0).getbbox()
        if not bb: continue   # nothing drawn (a decoration hidden in this state)
        im = im.crop(bb); ox, oy = bx - m + bb[0] / scale - cen[c][0], by - m + bb[1] / scale - cen[c][1]
        out.append((c, part, state, im, (ox, oy)))
    return out


def pack(sprites, width=2048):   # shelf packing, tallest first, 2 texels apart (the sampler's filter never reaches a neighbour); returns the atlas and each sprite's rectangle
    from PIL import Image
    order = sorted(range(len(sprites)), key=lambda i: -sprites[i][3].height)
    rects, x, y, row = {}, 1, 1, 0
    for i in order:
        w, h = sprites[i][3].size
        if x + w + 1 > width: x, y, row = 1, y + row + 2, 0
        rects[i] = (x, y, w, h); x += w + 2; row = max(row, h)
    atlas = Image.new('RGBA', (width, y + row + 1))
    for i, (x, y, w, h) in rects.items(): atlas.paste(sprites[i][3], (x, y))
    return atlas, rects


def theme(name, exe):
    from PIL import Image
    folder = THEMES / name; rows, alpha = [], None
    with tempfile.TemporaryDirectory() as tmp:
        for lay, scale in LAYOUTS.items():
            d = Design(folder / f'{lay}.dc.html'); alpha = d.alpha
            sprites = render(d, scale, exe, Path(tmp))
            atlas, rects = pack(sprites)
            # 256 colours with their alphas (PNG colour type 3 + tRNS, which the app decodes: overlay_theme.c): a quarter of the size, banding only faintly in the glows
            atlas.quantize(256, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE).save(folder / f'{lay}.png', optimize=True)
            rows.append(f'layout\t{lay}\t{d.W}\t{d.H}\t{lay}.png\t{scale}\t{d.travel:g}')
            for i, (c, part, state, im, (ox, oy)) in enumerate(sprites):
                x, y, w, h = rects[i]
                rows.append(f'sprite\t{lay}\t{c}\t{part}\t{state}\t{x}\t{y}\t{w}\t{h}\t{ox:.2f}\t{oy:.2f}')
            print(f'{name} {lay}: {len(sprites)} sprites, atlas {atlas.size[0]}x{atlas.size[1]}, {(folder / f"{lay}.png").stat().st_size / 1024:.0f} KB')
    head = ('# Generated by devtools/theme_render.py from tablet.dc.html and phone.dc.html; do not edit (nxnative/dm/android/themes/README.md)\n'
            '# alpha: the controls\' opacity. layout: <layout> <design width> <height> <atlas> <texels per CSS px> <knob travel, CSS px>\n'
            '# sprite: <layout> <control> <part> <state> <atlas x> <y> <w> <h> <top left from the control\'s centre, CSS px: x> <y>\n')
    (folder / 'theme.tsv').write_text(head + f'alpha\t{alpha:g}\n' + '\n'.join(rows) + '\n', encoding='utf-8', newline='\n')


def layout_tsv():   # the layouts from the ergo design: x from the nearer edge (or the middle) in window heights, y and sizes in heights too
    out = []
    for lay in LAYOUTS:
        d = Design(THEMES / 'ergo' / f'{lay}.dc.html'); cen = d.centres()
        for name, kind, btn, label in TABLE:
            cx, cy, w, h = cen[name]
            anchor = 0 if cx < d.W / 3 else 1 if cx > d.W * 2 / 3 else 0.5
            out.append(f'{lay}\t{name}\t{kind}\t{anchor:g}\t{(cx - anchor * d.W) / d.H:.4f}\t{cy / d.H:.4f}\t{w / d.H:.4f}\t{h / d.H:.4f}\t{btn}\t{label}')
    text = LAYOUT_TSV.read_text(encoding='utf-8'); head = ''.join(l + '\n' for l in text.splitlines() if l.startswith('#'))
    LAYOUT_TSV.write_text(head + '\n'.join(out) + '\n', encoding='utf-8', newline='\n'); print(f'{LAYOUT_TSV}: {len(out)} rows')


def preview(name, W, H, out, held=()):
    """A pack drawn as the app draws it (gfx_vk_present.c ov_theme_draw) in a W x H window, over a grey frame: where the layout puts each control and its sprites.
    held: controls (or d-pad directions) drawn held"""
    from PIL import Image
    lay = 'phone' if W / H >= 1.9 else 'tablet'   # overlay_layout.h OV_PHONE_ASPECT
    ctl = {c[1]: c for c in (l.split('	') for l in LAYOUT_TSV.read_text(encoding='utf-8').splitlines() if l and not l.startswith('#')) if c[0] == lay}
    rows = [l.split('	') for l in (THEMES / name / 'theme.tsv').read_text(encoding='utf-8').splitlines() if l and not l.startswith('#')]
    alpha = float(next(r[1] for r in rows if r[0] == 'alpha')); L = next(r for r in rows if r[0] == 'layout' and r[1] == lay)
    dh, scale, atlas = float(L[3]), float(L[5]), Image.open(THEMES / name / L[4]).convert('RGBA')
    k, img = H / dh, Image.new('RGBA', (W, H), (70, 74, 80, 255))
    def centre(c): return float(c[3]) * W + float(c[4]) * H, float(c[5]) * H
    def draw(r):
        x, y, w, h = map(int, r[5:9]); cx, cy = centre(ctl[r[2]]); sw, sh = max(1, round(w / scale * k)), max(1, round(h / scale * k))
        sp = atlas.crop((x, y, x + w, y + h)).resize((sw, sh), Image.LANCZOS); sp.putalpha(sp.getchannel('A').point(lambda v: int(v * alpha)))
        img.alpha_composite(sp, (round(cx + float(r[9]) * k), round(cy + float(r[10]) * k)))
    sp = [r for r in rows if r[0] == 'sprite' and r[1] == lay]
    for r in sp:
        if r[4] == 'decor': draw(r)
    for name_ in ctl:
        for r in sp:
            if r[2] != name_ or r[4] == 'decor': continue
            on = (r[3] if r[2] == 'dpad' else r[2]) in held
            if r[4] == 'knob' or r[4] == ('held' if on else 'idle'): draw(r)
    img.save(out); print(f'{out}: {name}, {lay} layout')


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--import', dest='src'); ap.add_argument('--only'); ap.add_argument('--browser'); ap.add_argument('--layout', action='store_true')
    ap.add_argument('--preview', nargs=3, metavar=('PACK', 'WxH', 'OUT.png'), help='draw a pack as the app does in a window of that size, and stop')
    ap.add_argument('--held', default='', help='with --preview: controls drawn held, comma-separated (a, zl, up, ...)')
    a = ap.parse_args()
    if a.preview: w, h = map(int, a.preview[1].lower().split('x')); preview(a.preview[0], w, h, a.preview[2], a.held.split(',')); return
    if a.src:
        for f in sorted(Path(a.src).glob('*.dc.html')):
            stem = f.name[:-len('.dc.html')]
            pk, lay = IMPORT.get(stem) or (stem.rsplit('-', 1) if stem.rsplit('-', 1)[-1] in LAYOUTS else (None, None))
            if not pk: print(f'skipped {f.name}'); continue
            (THEMES / pk).mkdir(parents=True, exist_ok=True); shutil.copyfile(f, THEMES / pk / f'{lay}.dc.html')
    if a.layout: layout_tsv()
    exe = browser(a.browser)
    for p in sorted(THEMES.iterdir()):
        if p.is_dir() and (not a.only or p.name == a.only) and (p / 'tablet.dc.html').is_file(): theme(p.name, exe)


if __name__ == '__main__': main()
