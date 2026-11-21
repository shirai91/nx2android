"""Shader reuse checks inputs, recorded failures and product integrity."""
import contextlib
import io
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from porter import pipeline, profiles, product_cache
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
        p.o = pipeline.Options(self.root, jobs=1); p.tools = {'dotnet': sys.executable, 'ndk': self.root / 'ndk'}
        tag = {'darwin':'darwin-x86_64', 'win32':'windows-x86_64'}.get(sys.platform, 'linux-x86_64')
        self.optimizer = p.tools['ndk'] / 'shader-tools' / tag / ('spirv-opt.exe' if os.name == 'nt' else 'spirv-opt')
        self.optimizer.parent.mkdir(parents=True); self.optimizer.write_bytes(b'optimizer')
        self.dll = self.root / 'translator/dmsh_xlate.dll'; self.dll.parent.mkdir(); self.dll.write_bytes(b'translator')
        self.exports = self.builds = 0; self.partial = self.empty = False; self.during_export = None
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
        self.last_export = argv
        self.exports += 1
        out = Path(argv[3])
        entries = [] if self.empty else [(1, 5, SPV, REFL)]
        dmsv.write(str(out), entries, level=0)
        expected = [['0000000000000001', 5]]
        failed = [['0000000000000001', 'no output']] if self.empty else []
        if self.partial:
            expected.append(['0000000000000002', 5])
            failed.append(['0000000000000002', 'no output'])
        if '--report' in argv:
            Path(argv[argv.index('--report') + 1]).write_text(json.dumps({
                'schema': 1, 'expected': expected,
                'failed': failed}))
        Path(logfile).write_text(f'{len(entries)} of {len(expected)} shaders -> bundle\n')
        if self.during_export: self.during_export()

    def export(self):
        with mock.patch.object(pipeline.Port, 'translator', return_value=(self.dll, 'source-key')):
            with contextlib.redirect_stdout(io.StringIO()): self.p.shaders()

    def test_numeric_pass_sources_invalidate_export_receipt(self):
        tools = self.root / 'dm/android/tools'; tools.mkdir(parents=True)
        names = ('spv_module.py', 'spv_builder.py', 'spv_half.py', 'spv_precise.py', 'spv_texture_offsets.py', 'spv_warp.py', 'spv_warp_flow.py', 'spv_warp_masks.py', 'spv_warp_predicated.py')
        for name in names: (tools / name).write_text('initial ' + name)
        with mock.patch.object(pipeline, 'DM', self.root / 'dm'):
            self.export()
            for number, name in enumerate(names, 2):
                with self.subTest(name=name):
                    (tools / name).write_text('changed ' + name)
                    self.export()
                    self.assertEqual(self.exports, number)

    def test_selected_ndk_optimizer_is_used_and_invalidates_receipt(self):
        self.export()
        self.assertIn('--spirv-opt', self.last_export)
        self.assertEqual(Path(self.last_export[self.last_export.index('--spirv-opt') + 1]), self.optimizer)
        self.optimizer.write_bytes(b'new optimizer')
        self.export()
        self.assertEqual(self.exports, 2)

    def test_unchanged_bundle_reused(self):
        self.export(); first = self.p.dmsv.read_bytes()
        self.export()
        self.assertEqual(self.exports, 1)
        self.assertEqual(self.p.dmsv.read_bytes(), first)

    def test_posix_same_size_corruption_rebuilt_even_with_original_mtime(self):
        product = self.root / 'bundle'; original = b'original'
        product.write_bytes(original)
        receipt = self.root / 'receipt'
        posix = SimpleNamespace(name='posix', fsync=os.fsync, replace=os.replace)
        st = SimpleNamespace(st_size=len(original), st_mtime_ns=1, st_ctime_ns=2,
                             st_dev=3, st_ino=4, st_mode=0o100600)
        actual_stat = Path.stat
        def stat_with_controlled_ctime(path, *args, **kwargs):
            return st if path == product else actual_stat(path, *args, **kwargs)
        with mock.patch.object(product_cache, 'os', posix), mock.patch.object(Path, 'stat', stat_with_controlled_ctime):
            product_cache.stamp(receipt, 'key', product, 'bundle')
            product.write_bytes(b'changed!')
            st.st_ctime_ns += 1
            self.assertIsNone(product_cache.reused(receipt, 'key', product))

    def test_empty_export_preserves_previous_products_and_receipt(self):
        self.partial = True
        self.export()
        published = [self.p.aw / name for name in ('shaders.dmsv', 'shaders.dmsv.report.json', 'shaders.dmsv.key')]
        original = [path.read_bytes() for path in published]
        (self.p.work / 'shader/pkg/cs.bin').write_bytes(b'changed shader input')
        self.partial = False
        self.empty = True
        with self.assertRaisesRegex(pipeline.BuildError, 'no shader translated'):
            self.export()
        self.assertEqual([path.read_bytes() for path in published], original)

    def test_partial_export_reused_with_failed_inventory(self):
        self.partial = True; self.export(); self.export()
        self.assertEqual(self.exports, 1)
        receipt = json.loads((self.p.aw / 'shaders.dmsv.key').read_text())
        self.assertFalse(receipt['complete'])
        self.assertIn('1 failed', receipt['note'])
        report = json.loads((self.p.aw / 'shaders.dmsv.report.json').read_text())
        self.assertEqual(report['failed'], [['0000000000000002', 'no output']])
        (self.p.work / 'shader/pkg/cs.bin').write_bytes(b'changed shader input')
        self.export()
        self.assertEqual(self.exports, 2)

    def test_unchanged_bundle_reuse_does_not_read_product_bytes(self):
        # Exercise the POSIX metadata path even on a Windows test runner.
        with mock.patch.object(product_cache, 'os', SimpleNamespace(name='posix', fsync=os.fsync, replace=os.replace)):
            self.export()
            original_open = Path.open
            def checked_open(path, *args, **kwargs):
                if path == self.p.dmsv: self.fail('unchanged bundle was read again')
                return original_open(path, *args, **kwargs)
            with mock.patch.object(Path, 'open', checked_open): self.export()
            self.assertEqual(self.exports, 1)

    def test_metadata_only_change_verified_once_then_reused(self):
        # Exercise the POSIX metadata path even on a Windows test runner.
        with mock.patch.object(product_cache, 'os', SimpleNamespace(name='posix', fsync=os.fsync, replace=os.replace)):
            self.export(); path = self.p.dmsv; st = path.stat()
            os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns + 1000000))
            self.export()
            self.assertEqual(self.exports, 1)
            original_open = Path.open
            def checked_open(candidate, *args, **kwargs):
                if candidate == path: self.fail('verified metadata change was not remembered')
                return original_open(candidate, *args, **kwargs)
            with mock.patch.object(Path, 'open', checked_open): self.export()

    def test_only_shader_export_helpers_invalidate_shader_key(self):
        dm = self.root / 'dm'; tools = dm / 'android/tools'; tools.mkdir(parents=True)
        names = ('shader_vk.py', 'dmsv.py', 'shader_products.py', 'android_profile.py', 'darwin2elf.py')
        for name in names: (tools / name).write_text('initial')
        with mock.patch.object(pipeline, 'DM', dm):
            self.export()
            for name in names[3:]: (tools / name).write_text('unrelated change')
            self.export(); self.assertEqual(self.exports, 1)
            for i, name in enumerate(names[:3], 2):
                (tools / name).write_text('export change')
                self.export(); self.assertEqual(self.exports, i)

    def test_failed_report_publication_does_not_stamp_reuse(self):
        self.partial = True
        replace = Path.replace
        def fail_report(path, target):
            if path.name == 'shaders.dmsv.partial.report.json': raise OSError('report publication failed')
            return replace(path, target)
        with mock.patch.object(Path, 'replace', fail_report), self.assertRaises(OSError): self.export()
        self.assertFalse((self.p.aw / 'shaders.dmsv.key').exists())
        self.export()
        self.assertEqual(self.exports, 2)
        report = json.loads((self.p.aw / 'shaders.dmsv.report.json').read_text())
        self.assertEqual(report['failed'], [['0000000000000002', 'no output']])

    def test_missing_or_changed_failed_report_prevents_reuse(self):
        self.partial = True; self.export()
        report = self.p.aw / 'shaders.dmsv.report.json'
        report.unlink(); self.export(); self.assertEqual(self.exports, 2)
        report.write_text('{}'); self.export(); self.assertEqual(self.exports, 3)
        self.assertEqual(json.loads(report.read_text())['failed'], [['0000000000000002', 'no output']])

    def test_windows_bundle_fast_path_and_hash_after_metadata_change(self):
        product = self.root / 'bundle'; product.write_bytes(b'original')
        receipt = self.root / 'receipt'
        windows = SimpleNamespace(name='nt', fsync=os.fsync, replace=os.replace)
        with mock.patch.object(product_cache, 'os', windows):
            product_cache.stamp(receipt, 'key', product, 'bundle')
            with mock.patch.object(product_cache, 'inventory', side_effect=AssertionError('unchanged bundle was hashed')):
                self.assertEqual(product_cache.reused(receipt, 'key', product), 'bundle')
            st = product.stat()
            product.write_bytes(b'changed!')
            os.utime(product, ns=(st.st_atime_ns, st.st_mtime_ns + 1_000_000))
            self.assertIsNone(product_cache.reused(receipt, 'key', product))

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
            report = self.p.aw / 'shaders.dmsv.partial.report.json'
            data = json.loads(report.read_text()); data['expected'].append(['0000000000000002', 5])
            report.write_text(json.dumps(data))
        self.during_export = omit
        with self.assertRaises(pipeline.BuildError): self.export()
        self.assertEqual(self.p.dmsv.read_bytes(), previous)

    def test_translator_unchanged_reused(self):
        self.p.translator(); self.p.translator()
        self.assertEqual(self.builds, 1)


if __name__ == '__main__': unittest.main()
