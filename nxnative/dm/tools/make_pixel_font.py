#!/usr/bin/env python3
# Convert a BDF bitmap font into the pack the Android overlay draws its text with (dm/android/pixel_font.h has the format and the reader).
# Usage: make_pixel_font.py font.bdf out.nxpf
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

if __name__ == '__main__':
    if len(sys.argv) != 3: sys.exit('usage: make_pixel_font.py font.bdf out.nxpf')
    open(sys.argv[2], 'wb').write(pack(glyphs(sys.argv[1])))
