#!/usr/bin/env python3
# Convert a BDF bitmap font into the pack the Android overlay draws its text with (dm/android/pixel_font.h has the format and the reader).
# Usage: make_pixel_font.py font.bdf out.nxpf
#        make_pixel_font.py --latin1 font.nxpf out.h   (the APK's pack cut to Latin-1 and U+FFFD, as a C header: the font built into the .so for an APK without one)
import struct, sys

def glyphs(path):
    g, cur, bits = {}, None, None
    for line in open(path, encoding='latin-1'):
        w = line.split()
        if not w: continue
        if w[0] == 'STARTCHAR': cur = {}
        elif w[0] == 'ENCODING': cur['cp'] = int(w[1])
        elif w[0] == 'DWIDTH': cur['adv'] = int(w[1])
        elif w[0] == 'BBX': cur['bbx'] = tuple(map(int, w[1:5]))
        elif w[0] == 'BITMAP': bits = []
        elif w[0] == 'ENDCHAR':
            if cur['cp'] >= 0: g[cur['cp']] = (cur['adv'], cur['bbx'], bytes.fromhex(''.join(bits)))
            bits = None
        elif bits is not None: bits.append(w[0])
    return g

def pack(g):
    index, data = [], bytearray()
    for cp in sorted(g):
        adv, (w, h, x, y), b = g[cp]
        if len(b) != (w + 7) // 8 * h: raise ValueError(f'U+{cp:04X}: {len(b)} bytes for a {w}x{h} glyph')
        index.append(struct.pack('<IIbbbb', cp << 8 | adv, len(data) << 8 | w, h, x, y, 0)); data += b
    return b'NXPF' + struct.pack('<I', len(index)) + b''.join(index) + bytes(data)

def unpack(b):   # a pack's glyphs, as glyphs() gives them
    n, g = struct.unpack_from('<I', b, 4)[0], {}
    for i in range(n):
        cp_adv, off_w, h, x, y, _ = struct.unpack_from('<IIbbbb', b, 8 + 12 * i); w, off = off_w & 255, 8 + 12 * n + (off_w >> 8)
        g[cp_adv >> 8] = (cp_adv & 255, (w, h, x, y), b[off:off + (w + 7) // 8 * h])
    return g

def latin1_header(b):
    sub = pack({cp: v for cp, v in unpack(b).items() if 32 <= cp < 256 or cp == 0xFFFD})
    rows = [', '.join(f'0x{c:02x}' for c in sub[i:i + 24]) for i in range(0, len(sub), 24)]
    return ('// Made by dm/tools/make_pixel_font.py --latin1 from dm/third_party/fusion-pixel-font (OFL 1.1, the licenses there): do not edit.\n'
            "// The overlay's font for an APK without assets/font/ui.bin (overlay_theme.c dm_overlay_font): Latin-1 and U+FFFD, in pixel_font.h's format\n"
            f'static const unsigned char PF_LATIN1[{len(sub)}] __attribute__((aligned(4))) = {{\n' + ''.join(f'    {r},\n' for r in rows) + '};\n')

if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == '--latin1':
        open(sys.argv[3], 'w', newline='\n').write(latin1_header(open(sys.argv[2], 'rb').read())); sys.exit()
    if len(sys.argv) != 3: sys.exit('usage: make_pixel_font.py font.bdf out.nxpf | --latin1 font.nxpf out.h')
    open(sys.argv[2], 'wb').write(pack(glyphs(sys.argv[1])))
