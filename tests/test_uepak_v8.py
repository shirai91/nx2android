"""Unreal Engine v8 pak and v1 shader archives through the production shader split."""
import importlib.util
import os
import ntpath
import posixpath
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
import zlib
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from porter.upstream import profile_guess, uepak  # noqa: E402

SPLIT = ROOT / 'nxnative/dm/gfx/tools/shader_split.py'
ARCHIVE = 'Content/ShaderArchive-Test-SF_GLSL_SWITCH.ushaderbytecode'


def shader():
    control = bytearray(0x880)
    control[:4] = bytes.fromhex('34127698')
    struct.pack_into('<I', control, 0x20, len(control))
    code = bytearray(0x70)
    code[:4] = bytes.fromhex('78563412')
    struct.pack_into('<I', code, 0x30, 1 << 10)
    struct.pack_into('<I', control, 0x704, len(code))
    return b'LSLGSV' + struct.pack('<I', len(code)) + code + struct.pack('<I', len(control)) + control


def archive_v1(record):
    compressed = zlib.compress(record)
    return struct.pack('<Ii', 1, 1) + bytes(20) + struct.pack('<QIIB', 0, len(compressed), len(record), 0) + compressed


def pak_fixture(path, archive, method_bytes=1, compress_pak=False, *, deleted=False, version=8):
    def fstr(text):
        text = text.encode() + b'\0'
        return struct.pack('<i', len(text)) + text

    method_fmt = 'B' if method_bytes == 1 else 'I'
    plain_header = 49 + method_bytes
    blob = zlib.compress(archive) if compress_pak else archive
    method = 2 if compress_pak else 0
    head = plain_header + 20
    blocks = struct.pack('<IQQ', 1, head, head + len(blob)) if method else b''

    payload = bytearray()
    index = bytearray(fstr('../../../') + struct.pack('<i', 2))
    # A non-shader entry precedes the archive to check entry boundaries and a nonzero file offset.
    for name, raw, data, slot, table, flags in [
        ('Content/Dummy.bin', b'placeholder', b'placeholder', 0, b'', 0),
        (ARCHIVE, archive, blob, method, blocks, 2 if deleted else 0),
    ]:
        entry = struct.pack('<QQ' + method_fmt + '20s', len(data), len(raw), slot, bytes(20)) + table
        entry += struct.pack('<BI', flags, len(raw) if slot else 0)
        index += fstr(name) + struct.pack('<Q', len(payload)) + entry
        payload += bytes(8) + entry + data
    methods = [b'Gzip', b'Zlib', b'', b''] + ([b''] if method_bytes == 4 else [])
    footer = struct.pack('<BIIQQ20s', 0, 0x5A6F12E1, version, len(payload), len(index), bytes(20))
    if version == 8:
        footer = bytes(16) + footer + b''.join(name.ljust(32, b'\0') for name in methods)
    path.write_bytes(payload + index + footer)


def nvn_main(path):
    image = bytearray(0x100)
    struct.pack_into('<I', image, 4, 0x10)
    image[0x10:0x14] = b'MOD0'
    struct.pack_into('<i', image, 0x14, 0x10)
    struct.pack_into('<qQqQ', image, 0x20, 6, 0x60, 5, 0x90)
    struct.pack_into('<I', image, 0x78, 1)
    image[0x91:0xA4] = b'nvnBootstrapLoader\0'
    header = bytearray(0x100)
    header[:4] = b'NSO0'
    struct.pack_into('<III4xIII4xIII', header, 0x10, 0x100, 0, 0x20, 0x120, 0x20, 0x20, 0x140, 0x40, 0xC0)
    struct.pack_into('<III', header, 0x60, 0x20, 0x20, 0xC0)
    path.write_bytes(header + image)


class UnrealV8(unittest.TestCase):
    def test_v8_shader_archive_runs_offline_through_split(self):
        for method_bytes, compress_pak in [(1, False), (4, True)]:
            with self.subTest(method_bytes=method_bytes), tempfile.TemporaryDirectory() as t:
                root = Path(t)
                romfs, exefs = root / 'RomFS', root / 'ExeFS'
                romfs.mkdir()
                exefs.mkdir()
                nvn_main(exefs / 'main')
                archive = archive_v1(shader())
                pak = romfs / 'Game-Switch.pak'
                pak_fixture(pak, archive, method_bytes, compress_pak)
                self.assertEqual(uepak.unreal_paks(str(romfs), legacy=True), [str(pak)])
                pack = uepak.Pak(pak)
                with pack.f:
                    self.assertEqual(pack.read(ARCHIVE), archive)
                sources = profile_guess.build_sources(exefs, romfs, ['main'])
                self.assertFalse(sources['ue_decode'][0])
                self.assertFalse(sources['ue_legacy'][0])
                profile = root / 'profile' / '0100000000AAA000'
                profile.mkdir(parents=True)
                (profile / 'game.tsv').write_text('shader_groups\tShaderArchive-=game\nshader_note\tSynthetic Unreal v8\n', encoding='utf-8')
                env = dict(os.environ, DM_GAME=profile.name, DM_GAMES_DIR=str(profile.parent), J='1', PYTHONUTF8='1',
                           DM_GLSLC_CAPTURE='', DM_UE_SHADERS='', DM_SPLIT_AOC='')
                args = [sys.executable, str(SPLIT), str(romfs), str(exefs / 'main'), str(root / 'pkg'),
                        str(root / 'map.tsv'), str(root / 'facts.tsv'), str(root / 'pairs.tsv')]
                run = subprocess.run(args, env=env, capture_output=True, text=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                row, = (root / 'map.tsv').read_text(encoding='utf-8').splitlines()[3:]
                self.assertTrue(row.startswith('vs_game.bin\t0\tvertex\t'), row)
                self.assertTrue(row.endswith(f'/Game-Switch.pak#{ARCHIVE}#UE0'), row)
                self.assertTrue((root / 'pkg/vs_game.bin').read_bytes().startswith(b'BTCDBTCD'))

    def test_legacy_labels_preserve_v4_windows_profile_paths(self):
        spec = importlib.util.spec_from_file_location('shader_split_legacy_labels', SPLIT)
        split = importlib.util.module_from_spec(spec)
        with mock.patch.dict(os.environ, {'DM_GAME': '0100000000010000'}):
            spec.loader.exec_module(split)
        record = shader()
        for version in (4, 8):
            for paths in (ntpath, posixpath):
                with self.subTest(version=version, paths=paths.__name__), tempfile.TemporaryDirectory() as tmp:
                    pak = Path(tmp) / 'Game-Switch.pak'
                    payload = struct.pack('<I', len(record)) + record if version == 4 else archive_v1(record)
                    pak_fixture(pak, payload, 4, version=version)
                    pack = uepak.Pak(pak)
                    with pack.f, mock.patch.dict(split._PAKS, {str(pak): pack}), mock.patch.object(split, 'os', SimpleNamespace(path=paths)):
                        output = []
                        split.ue_legacy_shaders(str(pak), pak.name, [ARCHIVE], output)
                    label, stage, control, code = output[0]
                    expected = '../../../' + ARCHIVE if version == 4 and paths is ntpath else ARCHIVE
                    if version == 4 and paths is ntpath:
                        expected = expected.replace('/', '\\')
                    self.assertEqual(label, f'/Game-Switch.pak#{expected}#UE{4 if version == 4 else 0}')
                    self.assertEqual(stage, 'vs')
                    self.assertEqual(len(control), 0x880)
                    self.assertEqual(len(code), 0x70)

    def test_archive_v1_checks_version_ranges_and_zlib_size(self):
        record = shader()
        valid = archive_v1(record)
        self.assertEqual(list(uepak.archive_v1_shaders(valid)), [(record, 0)])
        stored = struct.pack('<Ii', 1, 1) + bytes(20) + struct.pack('<QIIB', 0, len(record), len(record), 0) + record
        self.assertEqual(list(uepak.archive_v1_shaders(stored)), [(record, 0)])
        for offset, word, message in (
            (0, 3, 'not a version 1'),
            (4, 100, 'table size'),
            (28, len(valid), 'invalid data range'),
            (40, len(record) - 1, 'declared raw size'),
        ):
            with self.subTest(offset=offset):
                damaged = bytearray(valid)
                struct.pack_into('<I', damaged, offset, word)
                with self.assertRaisesRegex(ValueError, message):
                    list(uepak.archive_v1_shaders(damaged))

    def test_highest_patch_priority_selects_live_shader_archive(self):
        spec = importlib.util.spec_from_file_location('shader_split_pak_priority', SPLIT)
        split = importlib.util.module_from_spec(spec)
        with mock.patch.dict(os.environ, {'DM_GAME': '0100000000010000'}):
            spec.loader.exec_module(split)
        archive = archive_v1(shader())
        cases = (
            ('equal-priority move', 'pakchunk0-Switch_0_P.pak', 'pakchunk1-Switch_0_P.pak', True),
            ('lower-priority delete', 'pakchunk0-Switch_10_P.pak', 'pakchunk1-Switch_2_P.pak', True),
            ('higher-priority delete', 'pakchunk1-Switch_2_P.pak', 'pakchunk0-Switch_10_P.pak', False),
            ('patch deletes base', 'Game-Switch.pak', 'Game-Switch_P.pak', False),
        )
        for case, live, deleted, survives in cases:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                pak_fixture(root / live, archive)
                pak_fixture(root / deleted, b'', deleted=True)
                jobs, _ = split.ue_legacy_jobs([(str(root / name), name) for name in [live, deleted]])
                sources = [name for _, name, (_, entries) in jobs if ARCHIVE in entries]
                self.assertEqual(sources, [live] if survives else [])
        # Existing v4 inputs retain the lexical tie-break between patch paks.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            names = ['Game-Switch.pak', 'pakchunk0-Switch_10_P.pak', 'pakchunk1-Switch_2_P.pak']
            for name in names:
                pak_fixture(root / name, archive, 4, version=4)
            jobs, _ = split.ue_legacy_jobs([(str(root / name), name) for name in reversed(names)])
            self.assertEqual([name for _, name, (_, entries) in jobs if ARCHIVE in entries], [names[-1]])
