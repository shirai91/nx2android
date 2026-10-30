#!/usr/bin/env python3
# mkdex.py: the APK's one Java class as a classes.dex, with the Python standard library only (no JDK, no d8), for mkapk.py. Android gives the file picker's answer
# (the game data page's export and import) only to Activity.onActivityResult, which NativeActivity keeps to itself; this subclass keeps it in static fields for
# front_android.c (pick_wait):
#   public class org.nx2a.host.GameActivity extends android.app.NativeActivity {
#       public static volatile int requestCode, resultCode, resultSeq; public static volatile String resultUri;
#       public void onActivityResult(int req, int res, Intent data) {
#           String u = null; if (data != null) { Uri d = data.getData(); if (d != null) u = d.toString(); }
#           resultUri = u; resultCode = res; requestCode = req; resultSeq = resultSeq + 1;   // resultSeq last: a new answer is complete when the front end sees it change
#       }
#   }
# Dex format: https://source.android.com/docs/core/runtime/dex-format (version 035; every table sorted as the format requires, the map list, Adler-32 and SHA-1).
import hashlib, struct, zlib

CLASS = 'Lorg/nx2a/host/GameActivity;'
SUPER = 'Landroid/app/NativeActivity;'
INTENT, URI, STRING = 'Landroid/content/Intent;', 'Landroid/net/Uri;', 'Ljava/lang/String;'
FIELDS = [('requestCode', 'I'), ('resultCode', 'I'), ('resultSeq', 'I'), ('resultUri', STRING)]
PROTOS = [('V', 'V', ()), ('VIIL', 'V', ('I', 'I', INTENT)), ('L', URI, ()), ('L', STRING, ())]   # (shorty, return, parameters)
METHODS = [(SUPER, '<init>', 0), (CLASS, '<init>', 0), (CLASS, 'onActivityResult', 1), (INTENT, 'getData', 2), (URI, 'toString', 3)]   # (class, name, PROTOS index)
ACC_PUBLIC, ACC_STATIC, ACC_VOLATILE, ACC_CONSTRUCTOR = 0x1, 0x8, 0x40, 0x10000
NO_INDEX = 0xffffffff


def uleb(v):
    out = bytearray()
    while True:
        b = v & 0x7f; v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v: return bytes(out)


def mutf8(s):   # string_data_item: the length in UTF-16 units, then the (here ASCII) bytes and a 0
    return uleb(len(s)) + s.encode('ascii') + b'\0'


def dex():
    types = sorted({CLASS, SUPER, INTENT, URI, STRING, 'I', 'V'})
    strings = sorted(set(types) | {p[0] for p in PROTOS} | {n for n, _ in FIELDS} | {m[1] for m in METHODS})   # ASCII: byte order is the UTF-16 order the format asks for
    S = {s: i for i, s in enumerate(strings)}
    types.sort(key=lambda t: S[t]); T = {t: i for i, t in enumerate(types)}
    protos = sorted(range(len(PROTOS)), key=lambda i: (T[PROTOS[i][1]], [T[p] for p in PROTOS[i][2]]))
    P = {old: new for new, old in enumerate(protos)}
    fields = sorted(FIELDS, key=lambda f: (T[CLASS], S[f[0]], T[f[1]])); F = {f[0]: i for i, f in enumerate(fields)}
    methods = sorted(METHODS, key=lambda m: (T[m[0]], S[m[1]], P[m[2]])); M = {(m[0], m[1]): i for i, m in enumerate(methods)}

    # the two methods' code (registers: <init> v0 = this; onActivityResult v0 v1 locals, v2 this, v3 req, v4 res, v5 data)
    init = [0x1070, M[(SUPER, '<init>')], 0x0000,   # invoke-direct {v0}, NativeActivity.<init>()V
            0x000e]                                  # return-void
    oar = [0x0012,                                   # 0  const/4 v0, 0
           0x0538, 12,                               # 1  if-eqz v5, +12 (to 13)
           0x106e, M[(INTENT, 'getData')], 0x0005,   # 3  invoke-virtual {v5}, Intent.getData()Uri
           0x010c,                                   # 6  move-result-object v1
           0x0138, 6,                                # 7  if-eqz v1, +6 (to 13)
           0x106e, M[(URI, 'toString')], 0x0001,     # 9  invoke-virtual {v1}, Uri.toString()String
           0x000c,                                   # 12 move-result-object v0
           0x0069, F['resultUri'],                   # 13 sput-object v0, resultUri
           0x0467, F['resultCode'],                  # 15 sput v4, resultCode
           0x0367, F['requestCode'],                 # 17 sput v3, requestCode
           0x0160, F['resultSeq'],                   # 19 sget v1, resultSeq
           0x01d8, 0x0101,                           # 21 add-int/lit8 v1, v1, 1
           0x0167, F['resultSeq'],                   # 23 sput v1, resultSeq
           0x000e]                                   # 25 return-void
    def code_item(regs, ins, outs, insns): return struct.pack('<HHHHII', regs, ins, outs, 0, 0, len(insns)) + struct.pack(f'<{len(insns)}H', *insns)

    # layout: header, the id tables, then the data section (code items, type lists, string data, class data, the map list)
    n_ids = 0x70 + 4 * len(strings) + 4 * len(types) + 12 * len(PROTOS) + 8 * len(fields) + 8 * len(methods) + 32
    data = bytearray(); off = {}
    def put(key, b, align=1):
        while (n_ids + len(data)) % align: data.append(0)
        off[key] = n_ids + len(data); data.extend(b)
    put('code_init', code_item(1, 1, 1, init), 4)
    put('code_oar', code_item(6, 4, 1, oar), 4)
    for i in protos:
        if PROTOS[i][2]: put(('params', i), struct.pack('<I', len(PROTOS[i][2])) + struct.pack(f'<{len(PROTOS[i][2])}H', *[T[p] for p in PROTOS[i][2]]), 4)
    for s in strings: put(('str', s), mutf8(s))
    sf = b''.join(uleb(F[n] - (F[fields[k - 1][0]] if k else 0)) + uleb(ACC_PUBLIC | ACC_STATIC | ACC_VOLATILE) for k, (n, _) in enumerate(fields))
    put('class_data', uleb(len(fields)) + uleb(0) + uleb(1) + uleb(1) + sf +
        uleb(M[(CLASS, '<init>')]) + uleb(ACC_PUBLIC | ACC_CONSTRUCTOR) + uleb(off['code_init']) +
        uleb(M[(CLASS, 'onActivityResult')]) + uleb(ACC_PUBLIC) + uleb(off['code_oar']))
    n_params = sum(1 for i in protos if PROTOS[i][2])
    items = [(0x0000, 1, 0), (0x0001, len(strings), 0x70), (0x0002, len(types), 0x70 + 4 * len(strings)),
             (0x0003, len(PROTOS), 0x70 + 4 * len(strings) + 4 * len(types)),
             (0x0004, len(fields), 0x70 + 4 * len(strings) + 4 * len(types) + 12 * len(PROTOS)),
             (0x0005, len(methods), 0x70 + 4 * len(strings) + 4 * len(types) + 12 * len(PROTOS) + 8 * len(fields)),
             (0x0006, 1, n_ids - 32), (0x2001, 2, off['code_init']), (0x1001, n_params, min(v for k, v in off.items() if isinstance(k, tuple) and k[0] == 'params')),
             (0x2002, len(strings), off[('str', strings[0])]), (0x2000, 1, off['class_data'])]
    while (n_ids + len(data)) % 4: data.append(0)
    map_off = n_ids + len(data); items.append((0x1000, 1, map_off))   # the map list itself, last (the items are in offset order)
    data.extend(struct.pack('<I', len(items)) + b''.join(struct.pack('<HHII', t, 0, n, o) for t, n, o in items))

    ids = bytearray()
    ids += b''.join(struct.pack('<I', off[('str', s)]) for s in strings)
    ids += b''.join(struct.pack('<I', S[t]) for t in types)
    ids += b''.join(struct.pack('<III', S[PROTOS[i][0]], T[PROTOS[i][1]], off.get(('params', i), 0)) for i in protos)
    ids += b''.join(struct.pack('<HHI', T[CLASS], T[t], S[n]) for n, t in fields)
    ids += b''.join(struct.pack('<HHI', T[c], P[p], S[n]) for c, n, p in methods)
    ids += struct.pack('<IIIIIIII', T[CLASS], ACC_PUBLIC, T[SUPER], 0, NO_INDEX, 0, off['class_data'], 0)
    size = n_ids + len(data)
    header = struct.pack('<8sI20sIII', b'dex\n035\0', 0, b'\0' * 20, size, 0x70, 0x12345678) + struct.pack('<II', 0, 0) + struct.pack('<I', map_off)
    header += struct.pack('<IIIIIIIIIIII', len(strings), 0x70, len(types), 0x70 + 4 * len(strings), len(PROTOS), 0x70 + 4 * len(strings) + 4 * len(types),
                          len(fields), 0x70 + 4 * len(strings) + 4 * len(types) + 12 * len(PROTOS), len(methods),
                          0x70 + 4 * len(strings) + 4 * len(types) + 12 * len(PROTOS) + 8 * len(fields), 1, n_ids - 32)
    header += struct.pack('<II', size - n_ids, n_ids)
    assert len(header) == 0x70 and len(ids) == n_ids - 0x70
    b = bytearray(header + ids + data)
    b[12:32] = hashlib.sha1(b[32:]).digest()
    b[8:12] = struct.pack('<I', zlib.adler32(b[12:]))
    return bytes(b)


CLASS_NAME = CLASS[1:-1].replace('/', '.')   # the manifest's activity name

if __name__ == '__main__':
    import sys
    open(sys.argv[1], 'wb').write(dex())
