"""UnityFS bundles are unpacked one node at a time, and nodes that are not serialized files are never unpacked; built from synthetic bundles, without game files."""
import importlib.util
import mmap
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/gfx/tools'


def serialized_like(size, fill):
    # The 48-byte header is_serialized checks (format 21: metadata size, file size, 0, data offset), then filler.
    head = struct.pack('>4I', 64, size, 21, 64) + bytes(28)
    return head + bytes([fill]) * (size - len(head))


def bundle(nodes, block, kinds):
    """UnityFS format 7: nodes = [(name, bytes)] laid out back to back, cut into blocks of `block` bytes compressed by kinds[i % len(kinds)] (0 stored, 2 LZ4)."""
    import lz4.block
    stream = b''.join(data for _, data in nodes)
    blocks, raws = [], []
    for i in range(0, len(stream), block):
        u = stream[i:i + block]; kind = kinds[len(blocks) % len(kinds)]
        raw = lz4.block.compress(u, store_size=False) if kind == 2 else u
        blocks.append((len(u), len(raw), kind)); raws.append(raw)
    info = bytes(16) + struct.pack('>i', len(blocks)) + b''.join(struct.pack('>IIH', *b) for b in blocks)
    info += struct.pack('>i', len(nodes))
    off = 0
    for name, data in nodes:
        info += struct.pack('>qqI', off, len(data), 4) + name.encode() + b'\0'; off += len(data)
    head = b'UnityFS\0' + struct.pack('>I', 7) + b'5.x.x\0' + b'2018.4.24f1\0'
    head += struct.pack('>qIII', 0, len(info), len(info), 0x40)   # block info stored (kind 0), right after the header
    head += bytes(-len(head) % 16)
    return head + info + b''.join(raws), stream


def unpacked_whole(unityfs, d):   # the reader before bundle_nodes: the whole stream unpacked, then sliced
    p, blocks, nodes = unityfs._bundle_layout(d)
    out = bytearray()
    for u, c, f in blocks:
        out += unityfs._decomp(d[p:p + c], u, f & 0x3f); p += c
    return [(name, bytes(out[off:off + sz])) for off, sz, name in nodes]


@unittest.skipUnless(importlib.util.find_spec('lz4'), 'no lz4')
class BundleNodes(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(TOOLS))
        import unityfs
        cls.u = unityfs
        # the .resS bytes: no two 64-byte blocks alike, so each compressed block names its index
        cls.nodes = [('CAB-a', serialized_like(300, 1)), ('CAB-a.resS', bytes((i * 7 + i // 251) % 256 for i in range(2304))), ('CAB-b', serialized_like(250, 2)), ('tiny', b'xy')]

    def test_same_nodes_as_unpacking_the_whole_bundle(self):
        for block in (64, 100, 4096):
            for kinds in ((0,), (2,), (2, 0)):
                with self.subTest(block=block, kinds=kinds):
                    d, _ = bundle(self.nodes, block, kinds)
                    self.assertEqual(list(self.u.bundle_nodes(d)), unpacked_whole(self.u, d))
                    self.assertEqual(self.u.bundle_files(d), self.nodes)

    def test_keep_skips_nodes_that_are_not_serialized_files_without_unpacking_them(self):
        d, _ = bundle(self.nodes, 64, (2,))
        p, blocks, _ = self.u._bundle_layout(d)
        raws, c0 = [], p
        for u, c, f in blocks: raws.append(d[c0:c0 + c]); c0 += c
        res_start, res_end = 300, 300 + len(self.nodes[1][1])
        inside = {i for i in range(len(blocks)) if i * 64 >= res_start + 48 and (i + 1) * 64 <= res_end}   # wholly inside the .resS, past the 48 bytes keep looks at
        hidden = {raws[i] for i in inside}
        self.assertGreater(len(inside), 30)
        self.assertEqual(len(hidden), len(inside))
        self.assertFalse(hidden & {raws[i] for i in range(len(raws)) if i not in inside})
        unpacked = []
        real = self.u._decomp
        with patch.object(self.u, '_decomp', side_effect=lambda raw, size, kind: unpacked.append(bytes(raw)) or real(raw, size, kind)):
            got = list(self.u.bundle_nodes(d, self.u.serialized_head))
        self.assertEqual(got, [self.nodes[0], self.nodes[2]])
        self.assertFalse(hidden & set(unpacked))

    def test_serialized_head_matches_is_serialized(self):
        for data in (serialized_like(300, 1), serialized_like(300, 1)[:299], bytes(300), b'short'):
            self.assertEqual(self.u.serialized_head(data[:48], len(data)), self.u.is_serialized(data))

    def test_reads_a_mapped_file(self):
        d, _ = bundle(self.nodes, 100, (2, 0))
        with tempfile.TemporaryDirectory() as t:
            path = os.path.join(t, 'data.unity3d')
            with open(path, 'wb') as f: f.write(d)
            with open(path, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
                self.assertEqual(list(self.u.bundle_nodes(m)), self.nodes)


@unittest.skipUnless(importlib.util.find_spec('lz4'), 'no lz4')
class ScanFile(unittest.TestCase):
    def test_a_bundle_is_mapped_not_read(self):
        sys.path.insert(0, str(TOOLS))
        spec = importlib.util.spec_from_file_location('split_bundle_memory', TOOLS / 'shader_split.py')
        s = importlib.util.module_from_spec(spec)
        with patch.dict(os.environ, {'DM_GAME': '0100000000010000'}): spec.loader.exec_module(s)
        d, _ = bundle([('CAB-a', serialized_like(300, 1))], 64, (2,))
        seen = []
        with tempfile.TemporaryDirectory() as t:
            path = os.path.join(t, 'data.unity3d')
            with open(path, 'wb') as f: f.write(d)
            with patch.object(s, 'unity_shaders', side_effect=lambda d, label, out, trees: seen.append((type(d), d[:8], label))):
                self.assertEqual(s.scan_file((path, 'Data/data.unity3d')), ('Data/data.unity3d', {}, set(), {}))
        self.assertEqual(seen, [(mmap.mmap, b'UnityFS\0', '/Data/data.unity3d')])


if __name__ == '__main__': unittest.main()
