"""The GLSLC outputs a game keeps in a Unity TextAsset (FINAL FANTASY CRYSTAL CHRONICLES Remastered Edition, Unity 2018.4: shader.switch.vfxsb), read by unityfs.text_assets
and shader_split.unity_text_glslc. Synthetic serialized files only, no game data.
Adapted from seedds/nxnative's test_unity_text_glslc.py (MIT)."""
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
    _spec = importlib.util.spec_from_file_location('split_unity_text_glslc', TOOLS / 'shader_split.py')
    ss = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(ss)


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
