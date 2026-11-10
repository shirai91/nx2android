"""Cached shader products are reusable only while their actual bytes still match."""
import contextlib
import io
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from porter import pipeline, profiles
from porter.upstream import nsp
import dmsv

from test_shader_export_checks import module
SPV = module('cs')
REFL = struct.pack('<5I', 0, 0, 1, 1, 1)


class ShaderCache(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='nx2a_shader_cache_')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.p = pipeline.Port.__new__(pipeline.Port)
        p = self.p
        p.work = self.root / 'work'; p.aw = p.work / 'android'; p.logs = self.root / 'logs'
        p.aw.mkdir(parents=True); p.logs.mkdir()
        (p.work / 'shader/pkg').mkdir(parents=True)
        (p.work / 'shader/pkg/cs.bin').write_bytes(b'shader source')
        p.versions = ['v0']; p.dirs = {'v0': self.root / 'extracted'}
        p.g = pipeline.game.load('010043B013C5C000')
        p.env = dict(os.environ); p.derived = p.provisional = False; p.oodle_why = ''; p.aoc = []
        p.o = pipeline.Options(self.root, jobs=1); p.tools = {'dotnet': sys.executable}
        self.dll = self.root / 'translator/dmsh_xlate.dll'; self.dll.parent.mkdir(); self.dll.write_bytes(b'translator')
        self.exports = self.builds = 0; self.partial = False; self.during_export = None
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(nsp, 'split_shaders').start()
        mock.patch.object(pipeline.Port, 'device_oodle_end').start()
        mock.patch.object(nsp, 'run', side_effect=self.run_external).start()
        mock.patch.object(pipeline.paths, 'user_dir', return_value=self.root / 'user').start()

    def run_external(self, argv, logfile, **kwargs):
        if 'build' in argv:
            self.builds += 1
            out = Path(argv[argv.index('-o') + 1]); out.mkdir(parents=True, exist_ok=True)
            (out / 'dmsh_xlate.dll').write_bytes(b'compiler')
            (out / 'dependency.dll').write_bytes(b'dependency')
            (out / 'dmsh_xlate.runtimeconfig.json').write_text('{}')
            return
        self.exports += 1
        out = Path(argv[3])
        dmsv.write(str(out), [(1, 5, SPV, REFL)], level=0)
        expected = [['0000000000000001', 5]]
        if self.partial: expected.append(['0000000000000002', 5])
        if '--report' in argv:
            Path(argv[argv.index('--report') + 1]).write_text(json.dumps({
                'schema': 1, 'expected': expected,
                'failed': [['0000000000000002', 'no output']] if self.partial else []}))
        Path(logfile).write_text(f'1 of {len(expected)} shaders -> bundle\n')
        if self.during_export: self.during_export()

    def export(self):
        with mock.patch.object(pipeline.Port, 'translator', return_value=(self.dll, 'source-key')):
            with contextlib.redirect_stdout(io.StringIO()): self.p.shaders()

    def test_unchanged_bundle_reused(self):
        self.export(); first = self.p.dmsv.read_bytes()
        self.export()
        self.assertEqual(self.exports, 1)
        self.assertEqual(self.p.dmsv.read_bytes(), first)

    def test_same_size_corruption_rebuilt_even_with_original_mtime(self):
        self.export(); path = self.p.dmsv; original = path.read_bytes(); st = path.stat()
        path.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
        os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns))
        self.export()
        self.assertEqual(self.exports, 2)
        self.assertEqual(path.read_bytes(), original)

    def test_incomplete_export_is_not_reused_as_complete(self):
        self.partial = True; self.export(); self.export()
        self.assertEqual(self.exports, 2)

    def test_inputs_changed_during_export_preserve_previous_bundle(self):
        self.export(); original = self.p.dmsv.read_bytes()
        source = self.p.work / 'shader/pkg/cs.bin'; source.write_bytes(b'changed before')
        self.during_export = lambda: source.write_bytes(b'changed during')
        with self.assertRaises(pipeline.BuildError): self.export()
        self.assertEqual(self.p.dmsv.read_bytes(), original)

    def test_shader_producer_binary_change_invalidates_bundle(self):
        self.export(); self.dll.write_bytes(b'translator changed'); self.export()
        self.assertEqual(self.exports, 2)

    def test_translator_dependency_mutation_rebuilds(self):
        dll, _ = self.p.translator(); dependency = dll.parent / 'dependency.dll'
        st = dependency.stat(); dependency.write_bytes(b'DEPENDENCY')
        os.utime(dependency, ns=(st.st_atime_ns, st.st_mtime_ns))
        self.p.translator()
        self.assertEqual(self.builds, 2)
        self.assertEqual(dependency.read_bytes(), b'dependency')

    def test_translator_missing_dependency_rebuilds(self):
        dll, _ = self.p.translator(); (dll.parent / 'dependency.dll').unlink()
        self.p.translator()
        self.assertEqual(self.builds, 2)

    def test_translator_extra_product_invalidates_receipt(self):
        dll, _ = self.p.translator(); (dll.parent / 'injected.dll').write_bytes(b'extra')
        self.p.translator()
        self.assertEqual(self.builds, 2)
        self.assertFalse((dll.parent / 'injected.dll').exists())

    def test_legacy_and_malformed_receipts_force_rebuild(self):
        for receipt in ('old-key\nold note\n', '{}', 'null', '{broken'):
            self.export(); before = self.exports
            (self.p.aw / 'shaders.dmsv.key').write_text(receipt)
            self.export()
            self.assertEqual(self.exports, before + 1)

    def test_deleted_bundle_rebuilt(self):
        self.export(); self.p.dmsv.unlink(); self.export()
        self.assertTrue(self.p.dmsv.is_file())
        self.assertEqual(self.exports, 2)

    def test_missing_inventory_entry_preserves_previous_bundle(self):
        self.export(); previous = self.p.dmsv.read_bytes()
        source = self.p.work / 'shader/pkg/cs.bin'; source.write_bytes(b'new shader source')
        def omit():
            report = self.p.aw / 'shaders.dmsv.report.json'
            data = json.loads(report.read_text()); data['expected'].append(['0000000000000002', 5])
            report.write_text(json.dumps(data))
        self.during_export = omit
        with self.assertRaises(pipeline.BuildError): self.export()
        self.assertEqual(self.p.dmsv.read_bytes(), previous)

    def test_translator_unchanged_reused(self):
        self.p.translator(); self.p.translator()
        self.assertEqual(self.builds, 1)


if __name__ == '__main__': unittest.main()
