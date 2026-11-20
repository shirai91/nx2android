import ntpath
import os
import random
import struct
import subprocess
import sys
import unittest
import zlib
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest.mock import patch

import lz4.block

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from porter.upstream import profile_guess, uepak
with patch.dict(os.environ, {'DM_GAME': '0100000000010000'}):
    import shader_split


def fstring(name):
    data = name.encode() + b'\0'
    return struct.pack('<i', len(data)) + data


def write_pak(path, members, *, version=11, method='', encrypted=False):
    """A small pak with one block per compressed member and a complete directory index."""
    data, entries, names = bytearray(), bytearray(), bytearray()
    for name, raw in members.items():
        payload = {'Zlib': zlib.compress, 'LZ4': lambda d: lz4.block.compress(d, store_size=False)}[method](raw) if method else raw
        start = len(data)
        header = struct.pack('<qqqi', start, len(payload), len(raw), bool(method)) + bytes(20)
        if method:
            block_start = start + 73 if version == 4 else 73
            header += struct.pack('<iqq', 1, block_start, block_start + len(payload))
        header += struct.pack('<BI', 0, len(raw) if method else 0)
        data += header + payload
        if version == 4:
            entries += fstring(name) + header
        else:
            names += fstring(name) + struct.pack('<i', len(entries))
            flags = 0xe0000000 | (0x800040 if method else 0)
            entries += struct.pack('<III', flags, start, len(raw))
            if method:
                entries += struct.pack('<I', len(payload))

    index = fstring('../../../') + struct.pack('<i', len(members))
    if version == 4:
        index += entries
    else:
        directory = struct.pack('<i', 1) + fstring('/') + struct.pack('<i', len(members)) + names
        index += struct.pack('<QiiQQ', 0, 0, 1, len(data), len(directory)) + bytes(20)
        index += struct.pack('<i', len(entries)) + entries + struct.pack('<i', 0)
        data += directory

    footer = struct.pack('<BIIQQ', encrypted, uepak.PAK_MAGIC, version, len(data), len(index)) + bytes(20)
    if version == 11:
        footer = bytes(16) + footer + method.encode().ljust(160, b'\0')
    path.write_bytes(data + index + footer)
    return path


def make_shader(prefix=b''):
    code = shader_split.GFX_MAGIC + bytes(124)
    control = bytearray(0x880)
    control[:4] = shader_split.CTL_MAGIC
    struct.pack_into('<I', control, 0x20, len(control))
    struct.pack_into('<I', control, 0x704, len(code))
    record = b'LSLGSV' + prefix + struct.pack('<I', len(code)) + code + struct.pack('<I', len(control)) + control
    return record, (bytes(control), code)


def byte_array(data):
    return struct.pack('<i', len(data)) + data


def lz4_record(raw, frequency=0):
    block = lz4.block.compress(raw, store_size=False)
    return byte_array(block) + struct.pack('<iB', len(raw), frequency)


def make_nso(path):
    image = bytearray(0x80)
    struct.pack_into('<I4si', image, 4, 8, b'MOD0', 8)
    struct.pack_into('<qQqQ', image, 0x10, 6, 0x40, 5, 0x58)
    header = bytearray(0x100)
    header[:4] = b'NSO0'
    struct.pack_into('<III', header, 0x10, 0x100, 0, len(image))
    struct.pack_into('<III', header, 0x30, 0x100 + len(image), len(image), 0)
    struct.pack_into('<I', header, 0x60, len(image))
    path.write_bytes(header + image)


class InlineShaderTest(unittest.TestCase):
    def test_lz4_shader_with_extended_literal_length(self):
        # A long unique literal prefix uses more than one LZ4 length-extension byte.
        raw, (control, code) = make_shader(random.Random(173).randbytes(600))
        record = lz4_record(raw)
        self.assertEqual(record[5], 255)
        found = []
        shader_split.ue_inline_shaders(record, 'asset', found)
        self.assertEqual(found, [('asset#UE4', 'vs', control, code)])

    def test_lz4_declared_size_is_bounded_before_allocation(self):
        raw, _ = make_shader()
        block = lz4.block.compress(raw, store_size=False)
        block += bytes(100_000)   # enough input to pass the LZ4 expansion ratio check
        record = byte_array(block) + struct.pack('<iB', 20 << 20, 0)
        found = []
        with patch.object(lz4.block, 'decompress', wraps=lz4.block.decompress) as decompress:
            shader_split.ue_inline_shaders(record, 'asset', found)
            decompress.assert_not_called()
        self.assertEqual(found, [])

    def test_lz4_rejects_incomplete_block_mismatched_size_and_wrong_frequency(self):
        raw, _ = make_shader()
        block = lz4.block.compress(raw, store_size=False)
        records = {
            'truncated': byte_array(block[:12]) + struct.pack('<iB', len(raw), 0),
            'wrong size': byte_array(block) + struct.pack('<iB', len(raw) + 1, 0),
            'wrong frequency': lz4_record(raw, 3),
        }
        for kind, record in records.items():
            with self.subTest(kind=kind):
                found = []
                shader_split.ue_inline_shaders(record, 'asset', found)
                self.assertEqual(found, [])

    def test_truncated_control_record_is_ignored(self):
        found = []
        shader_split.ue_inline_shaders(byte_array(b'LSLGSV\0\0' + shader_split.CTL_MAGIC), 'asset', found)
        self.assertEqual(found, [])

    def test_legacy_zlib_and_stored_shaders(self):
        raw, (control, code) = make_shader()
        found = []
        shader_split.ue_inline_shaders(byte_array(zlib.compress(raw)) + byte_array(raw), 'asset', found)
        self.assertEqual([row[1:] for row in found], [('vs', control, code)] * 2)


class PakShaderTest(unittest.TestCase):
    def setUp(self):
        temp = TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.romfs = self.root / 'RomFS'
        self.exefs = self.root / 'ExeFS'
        self.romfs.mkdir()
        self.exefs.mkdir()
        make_nso(self.exefs / 'main')

    def sources(self):
        return profile_guess.build_sources(self.exefs, self.romfs, ['main'])

    def test_v11_inline_jobs_apply_patch_member_before_scanning(self):
        raw, (control, code) = make_shader()
        export, global_cache = 'Material.uexp', 'Engine/GlobalShaderCache-SF_GLSL_SWITCH.bin'
        base = write_pak(self.romfs / 'base.pak', {export: b'old', global_cache: byte_array(raw)}, method='Zlib')
        update = write_pak(self.romfs / 'update_P.pak', {export: lz4_record(raw)}, method='LZ4')
        jobs, taken = shader_split.ue_legacy_jobs([(os.fspath(p), p.name) for p in (update, base)], parts=1)
        self.assertEqual(taken, {str(base), str(update)})
        found = []
        for path, rel, (_, names) in jobs:
            shader_split.ue_legacy_shaders(path, rel, names, found)
            self.addCleanup(shader_split._PAKS.pop(path).f.close)
        self.assertEqual(found, [(f'/base.pak#{global_cache}#UE4', 'vs', control, code),
                                 (f'/update_P.pak#{export}#UE4', 'vs', control, code)])

    def test_v4_patch_still_overrides_base(self):
        base = write_pak(self.romfs / 'base.pak', {'Material.uexp': b'old', 'Other.bin': b'old'}, version=4)
        update = write_pak(self.romfs / 'update_P.pak', {'Material.uexp': b'new'}, version=4)
        jobs, _ = shader_split.ue_legacy_jobs([(str(p), p.name) for p in (update, base)], parts=1)
        self.assertEqual([(rel, names) for _, rel, (_, names) in jobs],
                         [(base.name, ('Other.bin',)), (update.name, ('Material.uexp',))])

    def test_inline_pak_needs_no_game_decoder(self):
        write_pak(self.romfs / 'inline.pak', {'Material.uexp': b'test'}, method='Zlib')
        sources = self.sources()
        self.assertFalse(sources['ue_decode'][0])
        self.assertFalse(sources['ue_legacy'][0])

    def test_modern_archive_pak_needs_archive_extraction(self):
        path = write_pak(self.romfs / 'archive.pak', {'Material.uexp': b'test', 'Global.ushaderbytecode': b'test'})
        self.assertTrue(self.sources()['ue_decode'][0])
        self.assertEqual(shader_split.ue_legacy_jobs([(str(path), path.name)]), ([], set()))

    def test_hidden_archive_index_still_runs_extraction(self):
        write_pak(self.romfs / 'encrypted.pak', {'Material.uexp': b'test'}, encrypted=True)
        self.assertTrue(self.sources()['ue_decode'][0])

    def test_config_pak_does_not_trigger_archive_extraction(self):
        write_pak(self.romfs / 'config.pak', {'Config/Test.ini': b'test'})
        self.assertFalse(self.sources()['ue_decode'][0])

    def write_iostore_library(self):
        header = bytearray(144)
        header[:16] = uepak.TOC_MAGIC
        header[16] = 4
        struct.pack_into('<9I', header, 20, len(header), 1, 0, 12, 0, 32, 64 << 10, 0, 1)
        library_chunk = bytes(11) + bytes((uepak.IO_SHADER_LIBRARY,))
        (self.romfs / 'library.utoc').write_bytes(header + library_chunk + bytes(10))

    def test_iostore_libraries_trigger_archive_extraction_without_pak(self):
        self.write_iostore_library()
        self.assertTrue(self.sources()['ue_decode'][0])

    def split_archive_game(self, *versions, aoc=''):
        decoded = self.root / 'ue_shaders'
        decoded.mkdir()
        (decoded / 'archive.ushaderbytecode').write_bytes(struct.pack('<7I', 2, 0, 0, 0, 0, 0, 0))
        map_path = self.root / 'map.tsv'
        env = dict(os.environ, DM_GAME='0100000000010000', DM_UE_SHADERS=str(decoded), DM_SPLIT_AOC=str(aoc), J='1', PYTHONUTF8='1')
        result = subprocess.run([
            sys.executable, shader_split.__file__, ','.join(map(str, (self.romfs, *versions))),
            ','.join([str(self.exefs / 'main')] * (1 + len(versions))), str(self.root / 'pkg'),
            str(map_path), str(self.root / 'facts.tsv'),
        ], env=env, capture_output=True, encoding='utf-8')
        self.assertEqual(result.returncode, 0, result.stderr)
        return map_path.read_text(encoding='utf-8')

    def test_archive_pak_uses_archive_route_for_sibling_cooked_packages(self):
        raw, _ = make_shader()
        write_pak(self.romfs / 'archive.pak', {'Global.ushaderbytecode': b'in-pack archive'})
        other_folder = self.romfs / 'Cooked'
        other_folder.mkdir()
        write_pak(other_folder / 'assets.pak', {'Material.uexp': lz4_record(raw)})
        inline_version = self.root / 'later-version' / 'RomFS'
        inline_version.mkdir(parents=True)
        write_pak(inline_version / 'inline.pak', {'Material.uexp': lz4_record(raw)})
        shader_map = self.split_archive_game(inline_version)
        self.assertNotIn('/Cooked/assets.pak#', shader_map)
        self.assertIn('/later-version/inline.pak#Material.uexp#UE4', shader_map)

    def test_iostore_library_uses_archive_route_for_sibling_global_cache(self):
        self.write_iostore_library()
        raw, _ = make_shader()
        write_pak(self.romfs / 'cache.pak', {'Engine/GlobalShaderCache-SF_GLSL_SWITCH.bin': byte_array(raw)})
        self.assertNotIn('/cache.pak#', self.split_archive_game())

    def test_archive_add_on_keeps_independent_add_on_inline_shaders(self):
        aoc = self.root / 'aoc'
        inline, archive = aoc / '1', aoc / '2'
        inline.mkdir(parents=True)
        archive.mkdir()
        raw, _ = make_shader()
        write_pak(inline / 'inline.pak', {'Material.uexp': lz4_record(raw)})
        write_pak(archive / 'shader.pak', {'Global.ushaderbytecode': b'in-pack archive'})
        write_pak(archive / 'assets.pak', {'Other.uexp': byte_array(zlib.compress(b'LSLG'))})
        self.assertIn('/aoc/1/inline.pak#Material.uexp#UE4', self.split_archive_game(aoc=aoc))

    def test_inline_shader_label_uses_mounted_posix_path_on_windows(self):
        raw, _ = make_shader()
        path = str(write_pak(self.romfs / 'inline.pak', {'Engine/Material.uexp': lz4_record(raw)}))
        found = []
        with patch.object(shader_split, 'os', SimpleNamespace(path=ntpath)):
            shader_split.ue_legacy_shaders(path, 'inline.pak', ['Engine/Material.uexp'], found)
        self.addCleanup(shader_split._PAKS.pop(path).f.close)
        self.assertEqual([where for where, *_ in found], ['/inline.pak#Engine/Material.uexp#UE4'])


if __name__ == '__main__':
    unittest.main()
