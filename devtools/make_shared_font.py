#!/usr/bin/env python3
# A system font for Chinese or Korean text (porter port --shared-font) made from fonts the player already has, such as their phone's own: one static
# TrueType font, the first font's glyphs first (its Latin), then the characters each next font adds (Hangul, CJK).
# Why one merged, static font: on a Galaxy Z Fold7 the phone's Korean font alone (OneUISansKR-VF.ttf: Hangul only, no Latin, a variable font) as the
# shared font crashed Super Mario Odyssey at start (SIGSEGV in main, its font engine); Roboto's Latin with that font's Hangul at its default weight,
# merged into one static font, drew its language menu ("한국어" where Roboto alone gave "???").
# Usage:
#   make_shared_font.py -o korean.ttf LATIN.ttf HANGUL.ttf [MORE.ttf ...]
#   make_shared_font.py -o korean.ttf --adb [SERIAL]   (the phone's Roboto, then the first of its /system/fonts TrueType fonts with Hangul)
# Run it from a clone of this repository (Python 3.11+, requirements.txt): the player's zip has no devtools/.
# Then: porter port <game> --shared-font korean.ttf. Fonts with CFF outlines (.otf, Noto Sans CJK) and collections (.ttc) are refused: the shared
# font is one TrueType font. Needs fontTools (requirements.txt).
import argparse, os, re, subprocess, sys, tempfile
from pathlib import Path
from fontTools.ttLib import TTFont
from fontTools.ttLib.scaleUpem import scale_upem
from fontTools.merge import Merger
from fontTools.varLib import instancer

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from porter.toolchain import find_adb   # noqa: E402  (the adb from the Setup tab or porter config adb, which need not be on PATH)

DROP = ('GSUB', 'GPOS', 'GDEF', 'BASE', 'JSTF', 'MATH', 'STAT', 'vhea', 'vmtx', 'VORG', 'kern', 'morx', 'DSIG', 'meta')   # layout and vertical tables: not merged, not needed
LATIN_ON_DEVICE = ('RobotoStatic-Regular.ttf', 'Roboto-Regular.ttf')
COVER = (('Latin', 'Aa'), ('Hangul', '한국어'), ('Kanji', '日本語'))


class FontError(Exception):
    pass


def load(path):
    if path.lower().endswith(('.ttc', '.otc')): raise FontError(f'{path}: a font collection; give a single .ttf')
    f = TTFont(path)
    if 'glyf' not in f: raise FontError(f'{path}: no TrueType outlines (CFF): give a .ttf with glyf outlines')
    if 'fvar' in f:   # a variable font: its default instance (Regular for a weight axis), with no variation tables left
        f = instancer.instantiateVariableFont(f, {a.axisTag: a.defaultValue for a in f['fvar'].axes})
    for t in DROP:
        if t in f: del f[t]
    return f


def merge(paths, out, name='Shared Font (merged)'):
    fonts = [load(p) for p in paths]
    upem = fonts[0]['head'].unitsPerEm
    with tempfile.TemporaryDirectory() as tmp:
        parts = []
        for i, f in enumerate(fonts):
            if f['head'].unitsPerEm != upem: scale_upem(f, upem)
            p = os.path.join(tmp, f'{i}.ttf'); f.save(p); parts.append(p)
        m = Merger().merge(parts)   # a code point the first fonts map keeps their glyph
    if len(m.getGlyphOrder()) > 65535: raise FontError('more than 65,535 glyphs: leave a font out')
    for nid in (1, 3, 4, 6): m['name'].setName(name if nid != 6 else re.sub(r'[^A-Za-z0-9-]', '', name) or 'SharedFont', nid, 3, 1, 0x409)
    m.save(out)
    cmap = TTFont(out).getBestCmap()
    return {label: all(ord(c) in cmap for c in text) for label, text in COVER}, len(cmap)


def from_device(serial, tmp):
    exe = find_adb()
    if not exe: raise FontError("no adb: get it with the Setup tab's Download missing tools, or give its path with porter config adb")
    adb = [str(exe)] + (['-s', serial] if serial else [])
    names = subprocess.run(adb + ['shell', 'ls', '/system/fonts'], capture_output=True, text=True, check=True).stdout.split()
    latin = next((n for n in LATIN_ON_DEVICE if n in names), None)
    if not latin: raise FontError('the device has no Roboto in /system/fonts')
    korean = [n for n in names if n.lower().endswith('.ttf') and re.search(r'kr|korean|hangul|nanum', n, re.I)]
    picked = []
    for n in [latin] + korean:
        p = os.path.join(tmp, n); subprocess.run(adb + ['pull', '/system/fonts/' + n, p], capture_output=True, check=True)
        if n == latin: picked.append(p); continue
        try: has = 0xAC00 in TTFont(p).getBestCmap() and 'glyf' in TTFont(p)
        except Exception: has = False
        if has: picked.append(p); break
    if len(picked) < 2: raise FontError('the device has no TrueType font with Hangul in /system/fonts (tried: %s)' % (', '.join(korean) or 'none'))
    return picked


def main(argv=None):
    ap = argparse.ArgumentParser(description='Merge fonts into one static TrueType system font for porter --shared-font')
    ap.add_argument('fonts', nargs='*', help='TrueType fonts, the one whose Latin is wanted first')
    ap.add_argument('-o', '--out', required=True, help='the font to write (.ttf)')
    ap.add_argument('--adb', nargs='?', const='', metavar='SERIAL', help="take the connected Android device's own Roboto and Korean font")
    a = ap.parse_args(argv)
    try:
        with tempfile.TemporaryDirectory() as tmp:
            paths = from_device(a.adb, tmp) if a.adb is not None else a.fonts
            if len(paths) < 2: ap.error('give at least two fonts, or --adb')
            cover, n = merge(paths, a.out)
    except (FontError, subprocess.CalledProcessError, OSError) as e:
        print(f'make_shared_font: {e}', file=sys.stderr); return 1
    print(f'{a.out}: {n} characters; ' + ', '.join(f'{k} {"yes" if v else "no"}' for k, v in cover.items()))
    return 0


if __name__ == '__main__':
    sys.exit(main())
