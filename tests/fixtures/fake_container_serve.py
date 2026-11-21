#!/usr/bin/env python3
# A stand-in for `nx_extract --serve` (nxnative/dm/tools/nx_extract/Program.cs Serve): the same frames over stdin/stdout, a RomFS in memory
# (tests/test_romfs.py runs the real client against it on a machine with no container at hand).
import os, struct, sys

FILES = {'main': b'\x00\x01\x02\x03' * 4, 'data/a.bin': bytes(range(256)) * 3, 'data/Pack/b.arc': b'BTCD' + b'\0' * 4092, 'glslc/x.glslc': b'#version 1\n'}
DIRS = {'', 'data', 'data/Pack', 'glslc'}

def rx(n):
    b = b''
    while len(b) < n:
        c = sys.stdin.buffer.read(n - len(b))
        if not c: raise SystemExit(0)
        b += c
    return b

def tx(payload, status=0):
    sys.stdout.buffer.write(bytes([status]) + struct.pack('<I', len(payload)) + payload); sys.stdout.buffer.flush()

def path(b):
    n = struct.unpack_from('<H', b, 0)[0]
    return b[2:2 + n].decode(), b[2 + n:]

handles, out = {}, sys.stdout.buffer
out.write(b'\0'); out.flush()
while True:
    head = rx(5); op, n = head[0], struct.unpack_from('<I', head, 1)[0]; body = rx(n)
    try:
        if op == 1:
            p, _ = path(body); p = p.strip('/')
            if p in FILES: tx(bytes([0]) + struct.pack('<q', len(FILES[p])))
            elif p in DIRS: tx(bytes([1]) + struct.pack('<q', 0))
            else: raise FileNotFoundError(p)
        elif op == 2:
            p, _ = path(body); p = p.strip('/')
            if p not in DIRS: raise NotADirectoryError(p)
            names = set()
            for q in list(FILES) + list(DIRS):
                if not q or q == p: continue
                if p:
                    if q.startswith(p + '/') and '/' not in q[len(p) + 1:]: names.add(q[len(p) + 1:])
                else: names.add(q.split('/')[0])
            b = struct.pack('<i', len(sorted(names)))
            for name in sorted(names):
                rel = f'{p}/{name}' if p else name; nb = name.encode()
                b += struct.pack('<H', len(nb)) + nb + bytes([rel in DIRS]) + struct.pack('<q', 0 if rel in DIRS else len(FILES[rel]))
            tx(b)
        elif op == 3:
            p, _ = path(body); handles[len(handles) + 1] = p.strip('/'); tx(struct.pack('<i', len(handles)))
        elif op == 4:
            h, off, ln = struct.unpack_from('<iqi', body); data = FILES[handles[h]][off:off + ln]; tx(struct.pack('<i', len(data)) + data)
        elif op == 5:
            handles.pop(struct.unpack_from('<i', body)[0], None); tx(b'')
        elif op == 6:
            tx(b''); raise SystemExit(0)
        else: raise ValueError(f'unknown request {op}')
    except Exception as e:
        b = str(e).encode(); tx(b, 1)