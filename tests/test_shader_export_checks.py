"""Run the export orchestration with a controlled external translator."""
import ast
import collections
import concurrent.futures as cf
import contextlib
import hashlib
import importlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'nxnative/dm/android/tools'
sys.path.insert(0, str(TOOLS))
import dmsv


def module(stage):
    model = 5 if stage == 'cs' else 0
    w = [0x07230203, 0x10300, 0, 6, 0,
         2 << 16 | 17, 1, 3 << 16 | 14, 0, 1,
         5 << 16 | 15, model, 4, 0x6e69616d, 0,
         6 << 16 | 16, 4, 17, 1, 1, 1,
         2 << 16 | 19, 1, 3 << 16 | 33, 2, 1,
         5 << 16 | 54, 1, 4, 0, 2, 2 << 16 | 248, 5,
         1 << 16 | 253, 1 << 16 | 56]
    return struct.pack(f'<{len(w)}I', *w)


class ExportChecks(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='nx2a_export_check_'); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name); self.out = self.root / 'out.dmsv'; self.out.write_bytes(b'previous bundle')
        self.report = self.root / 'report.json'; self.stage = 'cs'; self.calls = 0
        self.behavior = 'ok'; self.reflect = True
        (self.root / 'pkg').mkdir()

    def translate(self, argv, **kwargs):
        self.calls += 1; jobs = json.loads(Path(argv[2]).read_text()); dest = Path(argv[3])
        for job in jobs:
            if self.behavior == 'second-missing' and self.calls == 2: continue
            key = job['key']
            (dest / f'{key}.spv').write_bytes(module(self.stage))
            if self.reflect: (dest / f'{key}.res.json').write_text('{}')
        return subprocess.CompletedProcess(argv, 17 if self.behavior == 'process-failed' else 0, '', 'injected translator failure' if self.behavior == 'process-failed' else '')

    def run_export(self):
        table = self.root / 'shader_map.tsv'
        table.write_text(f'{self.stage}_test.bin\t0\t0\t0000000000000001\t0\n')
        (self.root / 'pkg' / f'{self.stage}_test.bin').write_bytes(b'fixture')
        game = SimpleNamespace(table=lambda name: self.root / name)
        try: products = importlib.import_module('shader_products')
        except ModuleNotFoundError: products = None
        env = dict(collections=collections, cf=cf, os=os, json=json, struct=struct,
                   tempfile=tempfile, subprocess=subprocess, dmsv=dmsv, Path=Path,
                   shader_products=products, GAME=game, MAP=str(table), XLATE_DLL='translator.dll',
                   a=SimpleNamespace(pkg=str(self.root/'pkg'), out=str(self.out), capture=None, keys='',
                                     jobs=1, dotnet='dotnet', zstd=0, report=str(self.report)),
                   STAGES={'vs': 0, 'cs': 5}, kinds_all=collections.Counter(),
                   captured=lambda _: {}, records=lambda _: [b'fixture'],
                   job_for=lambda key, stage, rec, w: ({'key': key, 'attr': {}}, b''),
                   int_inputs=lambda _: {'0': 'uint'},
                   spirv_bindings=lambda _: ([], [1, 1, 1], {1: [0, 0, 0], 3: [0, 0, 0]}),
                   refl_bin=lambda cl,c1,hdr: struct.pack('<5I', 0, 0, *hdr),
                   classify=lambda *args: None)
        source = TOOLS / 'shader_vk.py'; tree = ast.parse(source.read_text())
        nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'main']
        exec(compile(ast.Module(body=nodes, type_ignores=[]), str(source), 'exec'), env)
        with mock.patch.dict(os.environ, {'WORK': str(self.root)}), mock.patch.object(subprocess, 'run', side_effect=self.translate), contextlib.redirect_stdout(io.StringIO()):
            env['main']()

    def test_failed_translator_process_preserves_previous_bundle(self):
        self.behavior = 'process-failed'
        with self.assertRaises((ValueError, RuntimeError)): self.run_export()
        self.assertEqual(self.out.read_bytes(), b'previous bundle')

    def test_missing_reflection_is_not_published_as_success(self):
        self.reflect = False
        with self.assertRaises((ValueError, RuntimeError)): self.run_export()
        self.assertEqual(self.out.read_bytes(), b'previous bundle')

    def test_second_pass_cannot_reuse_first_pass_shader(self):
        self.stage = 'vs'; self.behavior = 'second-missing'
        with self.assertRaises((ValueError, RuntimeError)): self.run_export()
        self.assertEqual(self.out.read_bytes(), b'previous bundle')

    def test_export_reports_exact_expected_inventory(self):
        self.run_export()
        self.assertTrue(self.report.is_file())
        report = json.loads(self.report.read_text())
        self.assertEqual(report['expected'], [['0000000000000001', 5]])
        self.assertEqual(report['failed'], [])
        with dmsv.Bundle(str(self.out)) as b:
            self.assertEqual([(x[0], x[1]) for x in b.entries], [(1, 5)])


class ProductValidation(unittest.TestCase):
    def test_module_header_framing_and_stage_rejected(self):
        import shader_products
        good = module('cs')
        cases = [good[:-1], b'NOPE'+good[4:], good[:4]+struct.pack('<I', 0x10400)+good[8:],
                 good[:16]+struct.pack('<I', 1)+good[20:], good+struct.pack('<I', 0),
                 good+struct.pack('<I', 5<<16|17)]
        for data in cases:
            with self.subTest(data=data[-8:]), self.assertRaises(ValueError): shader_products.validate_spirv(data, 5)
        with self.assertRaises(ValueError): shader_products.validate_spirv(good, 0)
        shader_products.validate_spirv(good, 5)

    def test_bundle_inventory_and_reflection_checked_in_both_versions(self):
        import shader_products
        report = {'schema': 1, 'expected': [['0000000000000001', 5]], 'failed': []}
        with tempfile.TemporaryDirectory(prefix='nx2a_shader_products_') as tmp:
            path = Path(tmp) / 'out.dmsv'
            for level in ([0, 3] if dmsv.zstd_mod() else [0]):
                for reflection, accepted in [(struct.pack('<5I', 0, 0, 1, 1, 1), True),
                                              (struct.pack('<5I', 1, 0, 1, 1, 1), False),
                                              (struct.pack('<5I', 0, 1, 1, 1, 1), False)]:
                    dmsv.write(str(path), [(1, 5, module('cs'), reflection)], level=level)
                    if accepted: self.assertEqual(shader_products.check_bundle(path, report), (1, 1, True))
                    else:
                        with self.assertRaises(ValueError): shader_products.check_bundle(path, report)
                dmsv.write(str(path), [(1, 5, module('cs'), struct.pack('<5I', 0, 0, 1, 1, 1))], level=level)
                for bad in ({'schema':1,'expected':[['0000000000000002',5]],'failed':[]},
                            {'schema':1,'expected':[['0000000000000001',5]]*2,'failed':[]},
                            {'schema':1,'expected':[['0000000000000001',5]],'failed':[['0000000000000001','missing']]}):
                    with self.assertRaises(ValueError): shader_products.check_bundle(path, bad)


if __name__ == '__main__': unittest.main()
