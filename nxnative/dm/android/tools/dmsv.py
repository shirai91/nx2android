#!/usr/bin/env python3
# The Vulkan shader bundle, shaders.dmsv: written by shader_vk.py, read by the Vulkan backend (dm/android/gfx_vk_shader.c), dumped by devtools/dmsv_extract.py and spv_support_use.py.
#   "DMSV", u32 version, u32 count, then count 32-byte entries {u64 key, u32 stage (NVN's numbers), u32 spv_off, u32 spv_len, u32 refl_off, u32 refl_len, u32 spv_size} sorted by key,
#   then each entry's SPIR-V and reflection (shader_vk.py refl_bin), in that order. spv_size 0: the SPIR-V is stored as it is, spv_len bytes (every entry of version 1, where the field
#   was spare and 0); otherwise it is one zstd frame of spv_len bytes, with its checksum, that decompresses to spv_size bytes (version 2). The reflection is never compressed: the host
#   reads it in place in the mapped file (vkshader_t b, c1), and it is 1% of a bundle.
# Version 2 (2026-11): the SPIR-V is 99% of a bundle (Pokémon Pokopia 775 of 777 MB, Legends: Z-A 1,089 of 1,102 MB, Legends: Arceus 352 of 357 MB) and each module
#   compresses 3.5-3.6x at zstd level 3 on its own, so the host can still decompress one shader at a time. A module that does not get smaller is stored as it is.
#   dmsv.py info <bundle>                              its version, shaders, SPIR-V stored and decompressed
#   dmsv.py pack <bundle> <out> [--level N] [--jobs N]  the same shaders in the other form (--level 0: version 1, uncompressed), every module read back and compared
import argparse, concurrent.futures as cf, mmap, os, stat, struct, sys, tempfile

MAGIC, HDR, ENT = b'DMSV', struct.Struct('<4sII'), struct.Struct('<QIIIIII')
# zstd level 3, zstd's own default (1 does as well on this SPIR-V, within 1%). Each module on its own, the whole SPIR-V of three bundles (Mac M4 Pro; MB/s: one thread's CPU time):
#   level        1      3      6      9     16     19
#   Pokopia    3.52x  3.48x  3.67x  3.69x  4.08x  4.83x   (775 MB: 220, 222, 211, 210, 190, 160 MB)
#   Z-A        3.63x  3.60x  3.75x  3.82x  4.76x  4.84x   (1,089 MB: 300, 303, 290, 285, -, 225 MB; 16 on a 27 MB sample)
#   Arceus     3.64x  3.61x  3.77x  3.83x    -    4.78x   (352 MB)
#   MB/s        88     68     18     11     2.1    1.3
# Level 3 adds about 2 s to the port of the largest bundle (Z-A, 12 threads); level 19 takes another 25% off for 0.5-2 minutes more, where the whole shader stage takes
# 20-60 s. Decompressing costs the same at every level (the host's reader: 1.1-1.2 GB/s on the Mac at 1, 3 and 19). shader_vk.py --zstd and `dmsv.py pack --level` take another level
LEVEL = 3


def zstd_mod():   # compression.zstd from Python 3.14, its backport before (pip install backports.zstd), the same API; None without either
    try: from compression import zstd
    except ImportError:
        try: from backports import zstd
        except ImportError: return None
    return zstd


def write(path, entries, level=LEVEL, jobs=None):
    """entries [(key, stage, spv, refl)] → a bundle at path: version 2 with each module's SPIR-V zstd-compressed at level, in jobs threads (zstd lets go of the GIL: 7x faster
    on 12 threads), or version 1 with level 0 or in a Python without zstd. Written as it goes (the old writer joined the whole bundle in memory, twice its size).
    Verified in a private sibling directory, then atomically replaces path. A write,
    verification or replace failure leaves the old bundle intact (shader_vk.py
    calls this directly, without the CLI repacker's former .partial protection).
    Returns (version, SPIR-V bytes, SPIR-V bytes stored)"""
    entries = sorted(entries, key=lambda e: e[0]); z = zstd_mod() if level else None
    if level and not z: print('[RED] this Python has no zstd (compression.zstd from Python 3.14, or pip install backports.zstd): the shader bundle is written uncompressed (version 1)', flush=True)
    if z:
        P = z.CompressionParameter; opt = {P.compression_level: level, P.checksum_flag: 1}
        def pack(spv):
            c = z.compress(spv, options=opt)
            return (c, len(spv)) if len(c) < len(spv) else (spv, 0)
        with cf.ThreadPoolExecutor(jobs or os.cpu_count() or 4) as ex: stored = list(ex.map(pack, [e[2] for e in entries], chunksize=8))
    else: stored = [(e[2], 0) for e in entries]
    ver, off, table = 2 if z else 1, HDR.size + ENT.size * len(entries), []
    for (key, st, spv, refl), (s, n) in zip(entries, stored):
        table.append(ENT.pack(key, st, off, len(s), off + len(s), len(refl), n)); off += len(s) + len(refl)
    # Same filesystem for atomic rename; a unique directory also isolates concurrent
    # exporters. This preserves nxnative's staged-export contract in the DMSV format.
    path = os.path.abspath(path)
    with tempfile.TemporaryDirectory(prefix='.dmsv-', dir=os.path.dirname(path)) as scratch:
        tmp = os.path.join(scratch, 'bundle.dmsv')
        with open(tmp, 'wb') as f:
            f.write(HDR.pack(MAGIC, ver, len(entries)) + b''.join(table))
            for (_, _, _, refl), (s, _) in zip(entries, stored): f.write(s); f.write(refl)
            f.flush(); os.fsync(f.fileno())
        with Bundle(tmp) as check:
            if check.version != ver or len(check.entries) != len(entries):
                raise ValueError('shader bundle header did not read back correctly; previous bundle retained')
            for expected, actual in zip(entries, check.entries):
                if actual[:2] != expected[:2] or check.spv(actual) != expected[2] or check.refl(actual) != expected[3]:
                    raise ValueError(f'shader {expected[0]:016x} did not read back correctly; previous bundle retained')
        # Close the verifier's mapping before replace, including on Windows.
        try: mode = stat.S_IMODE(os.stat(path).st_mode)
        except FileNotFoundError: pass
        else: os.chmod(tmp, mode)
        os.replace(tmp, path)
    return ver, sum(len(e[2]) for e in entries), sum(len(s) for s, _ in stored)


class Bundle:
    """A bundle mapped read-only: version, entries [(key, stage, spv_off, spv_len, refl_off, refl_len, spv_size)], spv(e) and refl(e) of an entry"""
    def __init__(self, path):
        self.path, self.f, self.m = path, open(path, 'rb'), b''
        try:
            self.m = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ) if os.fstat(self.f.fileno()).st_size else b''
            magic, self.version, n = HDR.unpack_from(self.m) if len(self.m) >= HDR.size else (b'', 0, 0)
            if magic != MAGIC or self.version not in (1, 2): raise ValueError(f'{path}: not a shader bundle of version 1 or 2 ({magic!r}, version {self.version})')
            if HDR.size + ENT.size * n > len(self.m): raise ValueError(f'{path}: {n} entries run past the file ({len(self.m)} bytes)')
            self.entries = [ENT.unpack_from(self.m, HDR.size + ENT.size * i) for i in range(n)]
            bad = [e for e in self.entries if e[2] + e[3] > len(self.m) or e[4] + e[5] > len(self.m)]
            if bad: raise ValueError(f'{path}: {len(bad)} shaders past the end of the file (a partial copy?), the first {bad[0][0]:016x}')
            self.z = None
        except BaseException:
            # __exit__ is not called when construction fails; release handles now
            # so Windows can remove staging while the exception traceback lives.
            self.close(); raise

    def spv(self, e):
        b = self.m[e[2]:e[2] + e[3]]
        if not e[6]: return b
        self.z = self.z or zstd_mod()
        if not self.z: raise RuntimeError('this Python has no zstd (compression.zstd from Python 3.14, or pip install backports.zstd) to read a version 2 shader bundle')
        out = self.z.decompress(b)
        if len(out) != e[6]: raise ValueError(f'{self.path}: shader {e[0]:016x} decompresses to {len(out)} bytes, its entry says {e[6]}')
        return out

    def refl(self, e): return self.m[e[4]:e[4] + e[5]]

    def close(self):
        if isinstance(self.m, mmap.mmap): self.m.close()
        self.f.close()

    def __enter__(self): return self
    def __exit__(self, *exc): self.close()


def main(argv=None):
    ap = argparse.ArgumentParser(description='The Vulkan shader bundle (shaders.dmsv): show it, or write it again in the other form'); sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('info').add_argument('bundle')
    p = sub.add_parser('pack'); p.add_argument('bundle'); p.add_argument('out')
    p.add_argument('--level', type=int, default=LEVEL, help=f'zstd level (default {LEVEL}); 0 writes version 1, uncompressed'); p.add_argument('--jobs', type=int)
    a = ap.parse_args(argv)
    with Bundle(a.bundle) as b:
        raw, stored, nz = sum(e[6] or e[3] for e in b.entries), sum(e[3] for e in b.entries), sum(1 for e in b.entries if e[6])
        print(f'{a.bundle}: version {b.version}, {len(b.entries):,} shaders ({nz:,} zstd-compressed), {len(b.m) / 2**20:,.1f} MB; SPIR-V {raw / 2**20:,.1f} MB, stored {stored / 2**20:,.1f} MB'
              f' ({raw / max(stored, 1):.2f}x); reflection {sum(e[5] for e in b.entries) / 2**20:,.1f} MB')
        if a.cmd == 'info': return 0
        entries = [(e[0], e[1], bytes(b.spv(e)), bytes(b.refl(e))) for e in b.entries]
    ver, raw, stored = write(a.out, entries, a.level, a.jobs)
    print(f'{a.out}: version {ver}, {len(entries):,} shaders, {os.path.getsize(a.out) / 2**20:,.1f} MB; SPIR-V {raw / 2**20:,.1f} MB, stored {stored / 2**20:,.1f} MB ({raw / max(stored, 1):.2f}x);'
          f' every module read back byte for byte')
    return 0


if __name__ == '__main__':
    sys.exit(main())
