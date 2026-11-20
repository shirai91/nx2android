"""Both path conventions must agree on the first source of a repeated shader."""
import builtins
import importlib.util
import ntpath
import os
from pathlib import Path
import posixpath
import struct
import sys
import tempfile
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/gfx/tools'


class SerialPool:
    def __init__(self, _workers, initializer, initargs):
        initializer(*initargs)

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        pass

    def map(self, func, items, **_kwargs):
        return map(func, items)


def compiler_output(unique_word):
    control = bytearray(0x880)
    control[:4] = bytes.fromhex('34127698')
    struct.pack_into('<I', control, 0x20, len(control))
    code = bytearray(0x70)
    code[:4] = bytes.fromhex('78563412')
    struct.pack_into('<II', code, 0x30, 1 << 10, unique_word)
    struct.pack_into('<I', control, 0x704, len(code))
    header = bytearray(0x90)
    header[:4] = bytes.fromhex('91688619')
    struct.pack_into('<I', header, 0x44, len(header) + len(control) + len(code))
    return header + control + code


def open_fixture(path, *args, **kwargs):
    # Port.run starts shader tools with PYTHONUTF8=1. This in-process path
    # simulation must preserve that text encoding on a Windows ANSI locale.
    mode = args[0] if args else kwargs.get('mode', 'r')
    if 'b' not in mode:
        kwargs.setdefault('encoding', 'utf-8')
    return builtins.open(os.fspath(path).replace('\\', '/'), *args, **kwargs)


class ShaderSourceOrder(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('source_order_shader_split', TOOLS / 'shader_split.py')
        cls.split = importlib.util.module_from_spec(spec)
        with mock.patch.dict(os.environ, {'DM_GAME': '0100000000010000'}):
            spec.loader.exec_module(cls.split)

    def split_tables(self, root, path, sep):
        out = root / ('windows' if sep == '\\' else 'posix')
        out.mkdir()
        files = ['map.tsv', 'facts.tsv', 'pairs.tsv']
        argv = ['shader_split.py', (root / 'RomFS').as_posix(), (root / 'main').as_posix(),
                (out / 'pkg').as_posix(), *((out / f).as_posix() for f in files)]
        split_os = mock.Mock(wraps=os)
        split_os.path, split_os.sep = path, sep
        split_os.walk = lambda directory: ((d.replace('\\', '/'), dirs, fs) for d, dirs, fs in os.walk(directory))
        with mock.patch.object(self.split.cf, 'ProcessPoolExecutor', SerialPool), mock.patch.object(self.split, 'os', split_os):
            with mock.patch.object(self.split, 'open', open_fixture, create=True), mock.patch.object(sys, 'argv', argv):
                with mock.patch.dict(os.environ, {'DM_SPLIT_AOC': (root / 'DLC').as_posix(), 'DM_GLSLC_CAPTURE': '', 'DM_UE_SHADERS': '', 'J': '1'}):
                    self.split.main()
        return [(out / f).read_bytes().splitlines() for f in files]

    def test_romfs_and_dlc_choose_the_same_source_on_windows_and_posix(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'main').write_bytes(b'NSO0' + bytes(0xFC))
            for directory, unique_word in [('RomFS', 1), ('DLC/1', 2)]:
                for folder in ['Part2', 'Part']:
                    source = root / directory / folder / 'effect.tec'
                    source.parent.mkdir(parents=True)
                    source.write_bytes(compiler_output(unique_word))
            posix = self.split_tables(root, posixpath, '/')
            windows = self.split_tables(root, ntpath, '\\')
            self.assertEqual(posix[1:], windows[1:], 'facts and variation pairs already agree')
            self.assertEqual(posix[0], windows[0], 'the same inputs must yield the same map')
            sources = [row.rsplit(b'\t', 1)[-1] for row in posix[0][3:]]
            self.assertEqual(sources, [b'/Part/effect.tec#GLSLC@0x0', b'/aoc/1/Part/effect.tec#GLSLC@0x0'])
