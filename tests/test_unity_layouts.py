"""The Shader / ComputeShader layouts unityfs.py writes out for stripped Unity players (KNOWN_TREES, one per type hash) and the GLSLC outputs a game keeps in a Unity
TextAsset (FINAL FANTASY CRYSTAL CHRONICLES Remastered Edition, Unity 2018.4: shader.switch.vfxsb). Synthetic serialized files only, no game data.
Adapted from seedds/nxnative's test_unity_known_trees.py, test_unity_2021.py, test_unity_compute.py, test_unity_legacy_layout.py and test_unity_text_glslc.py (MIT)."""
import importlib.util
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/gfx/tools'
sys.path.insert(0, str(TOOLS))
import unityfs

with patch.dict(os.environ, {'DM_GAME': '0100000000010000'}):   # shader_split reads a profile at import; any one in dm/games does
    _spec = importlib.util.spec_from_file_location('split_unity_layouts', TOOLS / 'shader_split.py')
    ss = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(ss)

# The Shader (class 48) and ComputeShader (class 72) hashes of the Unity versions read without a bundle's tree. An entry dropped from KNOWN_TREES stops the split on every stripped
# player of that version ("no bundle has a Shader type tree of the same hash to borrow")
SHADER_HASHES = {'5.5': '4496e93f2179252104401c8dda0a1751', '2017.4': '266d53113fa30d2b858f2768f92eaa14', '2018.3': '8db760073e1392061e4a36995f0735af',
                 '2019.4': 'ed2a06ce7fb8203f6efb5ab5e0ce20d5', '2020.3': 'a30e3dcf6c6d25153d6f64e5e41caf4e',
                 '2021.2': 'd8236c9bd87d85edb203acf352348124', '2021.3': 'f0c184272a05e54447e36366ec4876a6', '2022.3': 'ea424a5fc1e48db62144df956f9e1005',
                 '6000.0': '2ec7223dce3af4232b14ae1441bd7999', '6000.3': '57f0dd2d4ef24437b4655529a673a936'}
COMPUTE_HASHES = {'2017.4': '20291e020d10074c35d8286f5191593f', '2019.4': '1a95529ac2eb1a97cd9d2a4356bd722f',
                  '2020.3': 'd0171b849bb064b2424893e7d4809be5', '2021.3': 'abd9135b8ce83d043fef4e9ec7f53366', '2022.3': 'bfdf63ab0ce84d5a2c765ee560d90fe7',
                  '6000.3': '43a0948a3f403068c0b31bff90ba0c9a'}
# Node counts of AssetRipper's release trees, as the nxnative commits that added the layouts give them
NODES = {(48, '4496e93f2179252104401c8dda0a1751'): 1222, (48, 'd8236c9bd87d85edb203acf352348124'): 2174, (48, 'f0c184272a05e54447e36366ec4876a6'): 2304,
         (48, 'ed2a06ce7fb8203f6efb5ab5e0ce20d5'): 1481, (48, '57f0dd2d4ef24437b4655529a673a936'): 2306, (72, 'abd9135b8ce83d043fef4e9ec7f53366'): 144}


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
    def test_every_supported_hash_has_a_layout(self):
        for cid, hashes in ((unityfs.CLASS_SHADER, SHADER_HASHES), (unityfs.CLASS_COMPUTE_SHADER, COMPUTE_HASHES)):
            for version, h in hashes.items():
                with self.subTest(cid=cid, version=version): self.assertIn((cid, bytes.fromhex(h)), unityfs.KNOWN_TREES)
        self.assertEqual(len(unityfs.KNOWN_TREES), len(SHADER_HASHES) + len(COMPUTE_HASHES))

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
                le, doff, types, entries = unityfs._tables(data, {}, {72}, trees)
                _, start, size, ti = entries[0]
                r = unityfs.Reader(data[doff + start:doff + start + size], le); unityfs._read(r, types[ti][2]); self.assertEqual(r.p, size)


def glslc_output():
    """A whole GLSLC output glslc_shaders accepts: the magic, its length at +0x44, one control section (its length at +0x20, the code's at +0x704) and the code area after it,
    a vertex shader (SPH word 0x400: shader type 1)"""
    nctl, ncode, c = 0x850, 0x40, 0x100
    d = bytearray(c + nctl + ncode); d[:4] = bytes.fromhex('91688619'); struct.pack_into('<I', d, 0x44, len(d))
    d[c:c + 4] = ss.CTL_MAGIC; struct.pack_into('<I', d, c + 0x20, nctl); struct.pack_into('<I', d, c + 0x704, ncode)
    k = c + nctl; d[k:k + 4] = ss.GFX_MAGIC; struct.pack_into('<I', d, k + 0x30, 0x400)
    return bytes(d)


def text_file(assets):
    """A serialized file (format 17, no type tree) of TextAssets: [(name, script bytes)], or raw bytes for an object of those bytes"""
    blks = []
    for a in assets:
        if isinstance(a, bytes): blks.append(a); continue
        nb = a[0].encode(); b = struct.pack('<i', len(nb)) + nb; b += bytes(-len(b) % 4); blks.append(b + struct.pack('<i', len(a[1])) + a[1])
    meta = bytearray(b'2018.4.2f1\0' + struct.pack('<i', 0) + b'\0' + struct.pack('<ii', 1, unityfs.CLASS_TEXT_ASSET) + b'\0' + struct.pack('<h', 0) + bytes(16) + struct.pack('<i', len(blks)))
    start = 0
    for i, blk in enumerate(blks):
        meta += bytes(-len(meta) % 4) + struct.pack('<qIIi', i + 1, start, len(blk), 0); start += len(blk)
    doff = 20 + len(meta)
    return struct.pack('>IIII', len(meta), doff + start, 17, doff) + bytes(4) + bytes(meta) + b''.join(blks)


class TextAssetGlslc(unittest.TestCase):
    def test_name_and_bytes(self):
        got = list(unityfs.text_assets(text_file([('vfxsb', glslc_output()), ('notes', b'hello world')]), {}))
        self.assertEqual(got, [(1, 'vfxsb', glslc_output()), (2, 'notes', b'hello world')])

    def test_objects_too_short_for_the_layout(self):   # a name whose padding runs past the object, an object shorter than the two length fields: left out, not read past
        got = list(unityfs.text_assets(text_file([struct.pack('<i', 1) + b'ab\0\0\0', b'\0\0', ('vfxsb', glslc_output())]), {}))
        self.assertEqual([name for _, name, _ in got], ['vfxsb'])

    def test_only_the_textasset_holding_the_magic(self):
        with patch.object(ss, 'COMMON', {}):
            (where, st, ctl, code), = ss.unity_text_glslc(text_file([('vfxsb', glslc_output()), ('notes', b'hello world')]), 'resources.assets')
            self.assertEqual(ss.unity_text_glslc(text_file([('notes', b'hello world')]), 'resources.assets'), [])   # no magic: nothing, as before
        self.assertEqual((where, st, ctl[:4], code[:4]), ('resources.assets#vfxsb#GLSLC@0x0', 'vs', ss.CTL_MAGIC, ss.GFX_MAGIC))

    def test_scan_file_reads_a_serialized_file_and_a_bundle(self):
        asset = text_file([('vfxsb', glslc_output())])
        info = bytes(16) + struct.pack('>iIIHiqqI', 1, len(asset), len(asset), 0, 1, 0, len(asset), 0) + b'resources.assets\0'
        head = b'UnityFS\0' + struct.pack('>I', 6) + b'5.x.x\0' + b'2018.4.2f1\0'
        bundle = head + struct.pack('>qIII', len(head) + 20 + len(info) + len(asset), len(info), len(info), 0) + info + asset
        with tempfile.TemporaryDirectory() as tmp, patch.object(ss, 'COMMON', {}), patch.object(ss, 'TREES', {}):
            for name, data, where in (('resources.assets', asset, '/resources.assets#vfxsb#GLSLC@0x0'), ('shaders.bundle', bundle, '/shaders.bundle:resources.assets#vfxsb#GLSLC@0x0')):
                with self.subTest(name):
                    p = Path(tmp) / name; p.write_bytes(data)
                    rel, shaders, pairs, trees = ss.scan_file((str(p), name))
                    self.assertEqual(([row[:2] for row in shaders.values()], pairs, trees), ([(where, 'vs')], set(), {}))


if __name__ == '__main__': unittest.main()
