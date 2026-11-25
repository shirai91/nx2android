"""The ComputeShader layouts unityfs.py writes out for stripped Unity players (KNOWN_TREES, one per type hash). Synthetic serialized files only, no game data.
Adapted from seedds/nxnative's test_unity_known_trees.py and test_unity_compute.py (MIT)."""
import importlib.util
import os
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/gfx/tools'
sys.path.insert(0, str(TOOLS))
import unityfs

with patch.dict(os.environ, {'DM_GAME': '0100000000010000'}):   # shader_split reads a profile at import; any one in dm/games does
    _spec = importlib.util.spec_from_file_location('split_unity_compute_layouts', TOOLS / 'shader_split.py')
    ss = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(ss)

# The ComputeShader (class 72) hashes of the Unity versions read without a bundle's tree. Without an entry, the second pass of the split leaves the kernels of a stripped
# player of that version out ("ComputeShader objects that no bundle lends a type tree to and unityfs.py has no layout for")
COMPUTE_HASHES = {'2017.4': '20291e020d10074c35d8286f5191593f', '2019.4': '1a95529ac2eb1a97cd9d2a4356bd722f',
                  '2020.3': 'd0171b849bb064b2424893e7d4809be5', '2021.3': 'abd9135b8ce83d043fef4e9ec7f53366', '2022.3': 'bfdf63ab0ce84d5a2c765ee560d90fe7',
                  '6000.3': '43a0948a3f403068c0b31bff90ba0c9a'}
# Node counts of AssetRipper's release trees, as the nxnative commits that added the layouts give them
NODES = {(72, 'abd9135b8ce83d043fef4e9ec7f53366'): 144}


def count(n): return 1 + sum(count(k) for k in n.kids)


def string(value):
    raw = value.encode()
    return struct.pack('<i', len(raw)) + raw + bytes(-len(raw) % 4)


def serialized(version, cid, type_hash, payload, fmt=17):
    """A serialized file without type trees holding one object of class cid"""
    metadata = version + b'\0' + struct.pack('<i?iiBh', 38, False, 1, cid, 0, -1) + type_hash + struct.pack('<i', 1)
    metadata += bytes(-(20 + len(metadata)) % 4)
    metadata += struct.pack('<qIII', 1, 0, len(payload), 0)
    return (struct.pack('>IIII', len(metadata), 0x100 + len(payload), fmt, 0x100) + bytes(4) + metadata).ljust(0x100, b'\0') + payload


class KnownComputeTrees(unittest.TestCase):
    def test_every_supported_compute_hash_has_a_layout(self):
        for version, h in COMPUTE_HASHES.items():
            with self.subTest(version=version):
                self.assertIn((unityfs.CLASS_COMPUTE_SHADER, bytes.fromhex(h)), unityfs.KNOWN_TREES)
                self.assertEqual(unityfs.known_tree(unityfs.CLASS_COMPUTE_SHADER, bytes.fromhex(h)).type, 'ComputeShader')
        self.assertEqual({h.hex() for cid, h in unityfs.KNOWN_TREES if cid == unityfs.CLASS_COMPUTE_SHADER}, set(COMPUTE_HASHES.values()))

    def test_node_counts(self):
        for (cid, h), n in NODES.items():
            with self.subTest(cid=cid, hash=h): self.assertEqual(count(unityfs.known_tree(cid, bytes.fromhex(h))), n)


def compute_2021_3(platforms=1, code_padding=0):
    """A ComputeShader of the 2021.3.18f1 release tree: several platforms expose the alignment after each resourcesResolved bool, odd-sized code the byte vector's padding"""
    code = ss.CS_MAGIC + bytes(60 + code_padding)
    control = bytearray(0x880); control[:4] = ss.CTL_MAGIC
    struct.pack_into('<I', control, 0x20, len(control)); struct.pack_into('<I', control, 0x704, len(code)); struct.pack_into('<III', control, 0x748, 64, 1, 1)
    header = [0] * 6 + [-1] * 6 + [0] * 12
    header[11], header[17], header[23] = 0, len(control), len(code)
    program = struct.pack('<24i', *header) + control + code
    resource = string('Result') + string('ResultBuffer') + struct.pack('<iii', 0, -1, 1)
    kernel = (struct.pack('<iI', 1, 7) + struct.pack('<i', 1) + resource + struct.pack('<4i', 0, 0, 0, 0)
              + struct.pack('<i', len(program)) + program + bytes(-len(program) % 4) + struct.pack('<4Iq', 3, 64, 1, 1, -0x123456789ab))
    parent = (string('SkinVertices') + struct.pack('<i', 2) + string('') + kernel + string('SKIN_NORMALS') + kernel
              + struct.pack('<i', 1) + string('GLOBAL') + struct.pack('<i', 1) + string('SKIN_NORMALS'))
    cb = string('Params') + struct.pack('<ii', 64, 1) + string('Count') + struct.pack('<i4I', 0, 4, 2, 3, 4)
    variants = [struct.pack('<iii', 38 + i, 50, 1) + parent + struct.pack('<i', 1) + cb + b'\1\0\0\0' for i in range(platforms)]
    payload = string('Skinning2021') + struct.pack('<i', platforms) + b''.join(variants)
    h = bytes.fromhex(COMPUTE_HASHES['2021.3'])
    return serialized(b'2021.3.18f1', 72, h, payload), h, bytes(control), code


class Compute2021_3(unittest.TestCase):
    def test_stripped_kernel_variants_reach_the_split(self):
        data, h, control, code = compute_2021_3()
        with self.assertRaises(unityfs.NoTree): list(unityfs.serialized_objects(data, {}, {72}, {}))
        with patch.object(ss, 'COMMON', {}):
            shaders = ss.unity_compute(data, 'fixture', {(72, h): unityfs.known_tree(72, h)})
        self.assertEqual(shaders, [(f'fixture#Skinning2021#SkinVertices.{i}', 'cs', control, code) for i in range(2)])

    def test_platform_alignment_and_byte_code_padding(self):
        for padding in range(4):
            with self.subTest(padding=padding):
                data, h, _, _ = compute_2021_3(platforms=2, code_padding=padding)
                trees = {(72, h): unityfs.known_tree(72, h)}
                (_, _, obj), = unityfs.serialized_objects(data, {}, {72}, trees)
                self.assertEqual([v['targetRenderer'] for v in obj['variants']], [38, 39])
                for variant in obj['variants']:
                    self.assertTrue(variant['resourcesResolved'])
                    parent, = variant['kernels']
                    self.assertEqual((parent['globalKeywords'], parent['localKeywords']), (['GLOBAL'], ['SKIN_NORMALS']))
                    self.assertEqual([v['first'] for v in parent['variantMap']], ['', 'SKIN_NORMALS'])
                    for pair in parent['variantMap']:
                        k = pair['second']
                        self.assertEqual((k['cbVariantIndices'], k['cbs'][0]['generatedName'], k['threadGroupSize'], k['requirements']), ([7], 'ResultBuffer', [64, 1, 1], -0x123456789ab))
                    self.assertEqual(variant['constantBuffers'][0]['params'][0]['colCount'], 4)
                payload = data[0x100:]   # serialized() puts its one object at 0x100, up to the file's end
                r = unityfs.Reader(payload, True); unityfs._read(r, trees[(72, h)]); self.assertEqual(r.p, len(payload))


if __name__ == '__main__': unittest.main()
