"""The Shader layouts unityfs.py writes out for stripped Unity players (KNOWN_TREES, one per type hash). Synthetic serialized files only, no game data.
Adapted from seedds/nxnative's test_unity_known_trees.py, test_unity_2021.py and test_unity_legacy_layout.py (MIT)."""
from pathlib import Path
import struct
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/gfx/tools'
sys.path.insert(0, str(TOOLS))
import unityfs

# The Shader (class 48) hashes of the Unity versions read without a bundle's tree. An entry dropped from KNOWN_TREES stops the split on every stripped
# player of that version ("no bundle has a Shader type tree of the same hash to borrow")
SHADER_HASHES = {'5.5': '4496e93f2179252104401c8dda0a1751', '2017.4': '266d53113fa30d2b858f2768f92eaa14', '2018.3': '8db760073e1392061e4a36995f0735af',
                 '2019.4': 'ed2a06ce7fb8203f6efb5ab5e0ce20d5', '2020.3': 'a30e3dcf6c6d25153d6f64e5e41caf4e',
                 '2021.2': 'd8236c9bd87d85edb203acf352348124', '2021.3': 'f0c184272a05e54447e36366ec4876a6', '2022.3': 'ea424a5fc1e48db62144df956f9e1005',
                 '6000.0': '2ec7223dce3af4232b14ae1441bd7999', '6000.3': '57f0dd2d4ef24437b4655529a673a936'}
# Node counts of AssetRipper's release trees, as the nxnative commits that added the layouts give them
NODES = {(48, '4496e93f2179252104401c8dda0a1751'): 1222, (48, 'd8236c9bd87d85edb203acf352348124'): 2174, (48, 'f0c184272a05e54447e36366ec4876a6'): 2304,
         (48, 'ed2a06ce7fb8203f6efb5ab5e0ce20d5'): 1481, (48, '57f0dd2d4ef24437b4655529a673a936'): 2306}


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


class KnownTrees(unittest.TestCase):
    def test_every_supported_shader_hash_has_a_layout(self):
        for version, h in SHADER_HASHES.items():
            with self.subTest(version=version): self.assertIn((unityfs.CLASS_SHADER, bytes.fromhex(h)), unityfs.KNOWN_TREES)
        self.assertEqual({h.hex() for cid, h in unityfs.KNOWN_TREES if cid == unityfs.CLASS_SHADER}, set(SHADER_HASHES.values()))

    def test_every_layout_builds_its_root(self):
        for (cid, h), (root, _) in unityfs.KNOWN_TREES.items():
            with self.subTest(cid=cid, hash=h.hex()): self.assertEqual(unityfs.known_tree(cid, h).type, root)

    def test_node_counts(self):
        for (cid, h), n in NODES.items():
            with self.subTest(cid=cid, hash=h): self.assertEqual(count(unityfs.known_tree(cid, bytes.fromhex(h))), n)

    def test_6000_3_appends_the_local_identifier_to_6000_0(self):
        old = unityfs.known_tree(48, bytes.fromhex(SHADER_HASHES['6000.0'])); new = unityfs.known_tree(48, bytes.fromhex(SHADER_HASHES['6000.3']))
        self.assertEqual([k.name for k in new.kids], [k.name for k in old.kids] + ['m_AssetLocalIdentifierInFile'])
        self.assertEqual((new.kids[-1].type, new.kids[-1].flags), ('SInt64', 0))

    def test_2021_3_adds_stage_counts_to_2021_2(self):
        old = unityfs.known_tree(48, bytes.fromhex(SHADER_HASHES['2021.2'])); new = unityfs.known_tree(48, bytes.fromhex(SHADER_HASHES['2021.3']))
        names = [k.name for k in old.kids]; names.insert(names.index('compressedBlob') + 1, 'stageCounts')
        self.assertEqual([k.name for k in new.kids], names)

    def test_2017_4_drops_two_fields_of_2018_3(self):
        new = unityfs.known_tree(48, bytes.fromhex(SHADER_HASHES['2017.4']))
        self.assertNotIn('m_NonModifiableTextures', [k.name for k in new.kids])
        self.assertEqual(count(unityfs.known_tree(48, bytes.fromhex(SHADER_HASHES['2018.3']))) - count(new), 12)


class Shader2021_2(unittest.TestCase):
    def test_stripped_shader_reads_keyword_tables_and_segments(self):
        # 2021.2's parsed form adds the keyword names and flags. Empty subshaders and properties (no pass is read: the passes are checked against the release tree, not
        # here), the parsed shader's strings, arrays and aligned bool, one platform with one segment
        parsed = (struct.pack('<i', 0) * 4 + string('Legacy/Blit') + string('') + string('') + struct.pack('<i', 0) + struct.pack('<i', 0) + b'\1\0\0\0')
        arrays = struct.pack('<iI', 1, 38) + struct.pack('<iiI', 1, 1, 0) * 3 + struct.pack('<i', 0) * 3
        payload = string('Legacy') + parsed + arrays + b'\1\0\0\0'
        h = bytes.fromhex(SHADER_HASHES['2021.2']); tree = unityfs.known_tree(48, h)
        (_, _, shader), = unityfs.serialized_objects(serialized(b'2021.2.13f1', 48, h, payload), {}, {48}, {(48, h): tree})
        self.assertEqual((shader['m_ParsedForm']['m_Name'], shader['m_ParsedForm']['m_KeywordNames'], shader['m_ParsedForm']['m_KeywordFlags']), ('Legacy/Blit', [], b''))
        self.assertEqual((shader['platforms'], shader['offsets'], shader['m_ShaderIsBaked']), ([38], [[0]], True))
        r = unityfs.Reader(payload, True); unityfs._read(r, tree); self.assertEqual(r.p, len(payload))


class Shader5_5(unittest.TestCase):
    def test_legacy_fields(self):
        tree = unityfs.known_tree(48, bytes.fromhex(SHADER_HASHES['5.5']))
        fields = {n.name: n for n in tree.kids}
        self.assertNotIn('m_NonModifiableTextures', fields)
        self.assertEqual((fields['platforms'].kids[0].flags, fields['compressedBlob'].kids[0].flags), (0, 0x4000))
        sub = next(n for n in fields['m_ParsedForm'].kids if n.name == 'm_SubShaders').kids[0].kids[1]
        pas = next(n for n in sub.kids if n.name == 'm_Passes').kids[0].kids[1]
        self.assertNotIn('m_HasProceduralInstancingVariant', [n.name for n in pas.kids])
        sp = next(n for n in pas.kids if n.name == 'progVertex').kids[0].kids[0].kids[1]
        self.assertNotIn('m_ShaderRequirements', [n.name for n in sp.kids])
        bind = next(n for n in sp.kids if n.name == 'm_Channels')
        r = unityfs.Reader(struct.pack('<i', 1) + bytes([2, 3, 0, 0]) + struct.pack('<I', 7), True)   # one 2-byte channel pair, padded before m_SourceMap
        self.assertEqual(unityfs._read(r, bind)['m_SourceMap'], 7); self.assertEqual(r.p, 12)


if __name__ == '__main__': unittest.main()
