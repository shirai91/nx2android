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
import re
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


def reflection(key='0000000000000001', stage='cs'):
    return dict(key=key, stage=stage, cbufs=[], sbufs=[], textures=[], images=[])


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


def vertex_module(numeric='float', width=32, channels=4, location=11, component=0, id_offset=0, array_length=None):
    def ident(n): return n + id_offset
    i = ident
    scalar = (22, [i(1), width]) if numeric == 'float' else (21, [i(1), width, numeric == 'sint'])
    array = [] if array_length is None else [(21, [i(9), 32, 0]), (43, [i(9), i(10), array_length]), (28, [i(11), i(2), i(10)])]
    instructions = [(17, [1]), (14, [0, 1]), (15, [0, i(7), 0x6e69616d, 0, i(4)]),
                    (71, [i(4), 30, location]), (71, [i(4), 31, component]), scalar,
                    (23, [i(2), i(1), channels]), *array, (32, [i(3), 1, i(2) if array_length is None else i(11)]), (59, [i(3), i(4), 1]),
                    (19, [i(5)]), (33, [i(6), i(5)]), (54, [i(5), i(7), 0, i(6)]),
                    (248, [i(8)]), (253, []), (56, [])]
    words = [0x07230203, 0x10300, 0, i(9 if array_length is None else 12), 0]
    for op, args in instructions: words.extend([(len(args)+1)<<16|op, *args])
    return struct.pack(f'<{len(words)}I', *words)


class ExportChecks(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='nx2a_export_check_'); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name); self.out = self.root / 'out.dmsv'; self.out.write_bytes(b'previous bundle')
        self.report = self.root / 'report.json'; self.stage = 'cs'; self.calls = 0
        self.behavior = 'ok'; self.reflect = True; self.second_shader = False; self.bad_resource = None
        self.jobs = 1; self.bad_key = None; self.log = io.StringIO(); self.bindings = []
        self.job_history = []; self.inference = {'0': 'uint'}
        (self.root / 'pkg').mkdir()

    def translate(self, argv, **kwargs):
        self.calls += 1; jobs = json.loads(Path(argv[2]).read_text()); dest = Path(argv[3])
        self.job_history.extend(jobs)
        affected = self.bad_key is None or any(job['key'] == self.bad_key for job in jobs)
        if affected and self.behavior == 'launch-failed': raise OSError('translator unavailable')
        bad = 0
        for job in jobs:
            broken = self.bad_key is None or job['key'] == self.bad_key
            if self.behavior == 'partial' and job['key'].endswith('2'): bad += 1; continue
            if broken and self.behavior in ('process-failed', 'crash-after-output'): bad += 1; continue
            if broken and self.behavior == 'second-missing' and job['attr']: bad += 1; continue
            optimized = job.get('constant_buffer_address') is True
            if broken and self.behavior == 'optimized-missing' and optimized: bad += 1; continue
            key = job['key']
            spv = module(self.stage)
            if self.behavior == 'input-type-change': spv = vertex_module('sint' if optimized else 'float')
            (dest / f'{key}.spv').write_bytes(b'bad module' if broken and self.behavior == 'bad-module' else spv)
            if broken and self.behavior == 'failed-after-spv': bad += 1; continue
            resource = reflection(key, self.stage)
            if self.behavior != 'old-translator': resource['constant_buffer_address'] = job.get('constant_buffer_address', True)
            if self.reflect or not broken: (dest / f'{key}.res.json').write_text(json.dumps(self.bad_resource if broken and self.bad_resource is not None else resource))
        summary = f'translated: {len(jobs)-bad} ok, {bad} failed\n'
        if affected and self.behavior in ('process-failed', 'crash-after-output'):
            return subprocess.CompletedProcess(argv, 17, '', 'injected translator failure')
        return subprocess.CompletedProcess(argv, 1 if bad else 0, summary, '')

    def run_export(self):
        table = self.root / 'shader_map.tsv'
        table.write_text(f'{self.stage}_test.bin\t0\t0\t0000000000000001\t0\n' +
                         (f'{self.stage}_test.bin\t0\t0\t0000000000000002\t0\n' if self.second_shader else ''))
        (self.root / 'pkg' / f'{self.stage}_test.bin').write_bytes(b'fixture')
        game = SimpleNamespace(table=lambda name: self.root / name)
        try: products = importlib.import_module('shader_products')
        except ModuleNotFoundError: products = None
        env = dict(collections=collections, cf=cf, os=os, json=json, struct=struct,
                   tempfile=tempfile, subprocess=subprocess, dmsv=dmsv, Path=Path, re=re,
                   shader_products=products, GAME=game, MAP=str(table), XLATE_DLL='translator.dll',
                   a=SimpleNamespace(pkg=str(self.root/'pkg'), out=str(self.out), capture=None, keys='',
                                     jobs=self.jobs, dotnet='dotnet', zstd=0, report=str(self.report)),
                   STAGES={'vs': 0, 'cs': 5}, kinds_all=collections.Counter(),
                   captured=lambda _: {}, records=lambda _: [b'fixture'],
                   job_for=lambda key, stage, rec, w: ({'key': key, 'attr': {}}, b''),
                   int_inputs=lambda _: self.inference,
                   spirv_bindings=lambda _: (self.bindings, [1, 1, 1], {1: [0, 0, 0], 3: [0, 0, 0]}),
                   refl_bin=lambda cl,c1,hdr: struct.pack('<5I', 0, 0, *hdr),
                   classify=lambda *args: None)
        source = TOOLS / 'shader_vk.py'; tree = ast.parse(source.read_text(encoding='utf-8'))
        nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'main']
        exec(compile(ast.Module(body=nodes, type_ignores=[]), str(source), 'exec'), env)
        with mock.patch.dict(os.environ, {'WORK': str(self.root)}), mock.patch.object(subprocess, 'run', side_effect=self.translate), contextlib.redirect_stdout(self.log):
            env['main']()

    def test_normal_per_shader_failure_keeps_best_effort_export(self):
        self.behavior = 'partial'; self.second_shader = True
        self.run_export()
        report = json.loads(self.report.read_text())
        self.assertEqual(report['expected'], [['0000000000000001',5], ['0000000000000002',5]])
        self.assertEqual(report['failed'], [['0000000000000002','no output']])
        with dmsv.Bundle(str(self.out)) as bundle:
            self.assertEqual([e[0] for e in bundle.entries], [1])

    def assert_failed(self, key, reason, emitted):
        report = json.loads(self.report.read_text())
        self.assertEqual([row[0] for row in report['failed']], [key])
        self.assertIn(reason, report['failed'][0][1])
        self.assertIn('[RED] ' + key, self.log.getvalue())
        with dmsv.Bundle(str(self.out)) as bundle:
            self.assertEqual([e[0] for e in bundle.entries], emitted)

    def test_failed_translator_batch_does_not_stop_other_batches(self):
        self.second_shader = True; self.jobs = 2; self.bad_key = '0000000000000002'
        for behavior in ('process-failed', 'launch-failed'):
            with self.subTest(behavior=behavior):
                self.behavior = behavior; self.run_export()
                self.assert_failed(self.bad_key, 'translator', [1])

    def test_crashed_batch_retains_valid_output_from_same_batch(self):
        self.second_shader = True
        self.bad_key = '0000000000000002'
        self.behavior = 'crash-after-output'
        self.run_export()
        self.assert_failed(self.bad_key, 'translator', [1])

    def test_completed_batch_retains_valid_shader_when_failed_shader_left_spirv(self):
        self.second_shader = True
        self.bad_key = '0000000000000002'
        self.behavior = 'failed-after-spv'
        self.run_export()
        self.assert_failed(self.bad_key, 'omitted reflection', [1])

    def test_all_failed_shaders_keep_best_effort_empty_export(self):
        self.behavior = 'process-failed'; self.run_export()
        self.assert_failed('0000000000000001', 'translator', [])

    def test_missing_reflection_omits_only_bad_shader(self):
        self.second_shader = True; self.bad_key = '0000000000000002'; self.reflect = False
        self.run_export(); self.assert_failed(self.bad_key, 'omitted reflection', [1])

    def test_wrong_or_incomplete_reflection_omits_only_bad_shader(self):
        self.second_shader = True; self.bad_key = '0000000000000002'
        for bad in ({}, {**reflection(), 'key': 'wrong'}, {**reflection(self.bad_key), 'stage': 'vs'},
                    {**reflection(self.bad_key), 'sbufs': [{}]}, {**reflection(self.bad_key), 'images': None}):
            with self.subTest(resource=bad):
                self.bad_resource = bad; self.run_export()
                self.assert_failed(self.bad_key, 'reflection', [1])

    def test_malformed_module_omits_only_bad_shader(self):
        self.second_shader = True; self.bad_key = '0000000000000002'; self.behavior = 'bad-module'
        for self.stage in ('cs', 'vs'):
            with self.subTest(stage=self.stage):
                self.run_export(); self.assert_failed(self.bad_key, 'SPIR-V', [1])

    def test_binding_validation_failure_is_reported(self):
        self.bindings = [dict(name='cp_s0', binding=3, type='ssbo')]
        self.run_export(); self.assert_failed('0000000000000001', 'reflection', [])

    def test_second_pass_cannot_reuse_first_pass_shader(self):
        self.stage = 'vs'; self.behavior = 'second-missing'
        self.second_shader = True; self.bad_key = '0000000000000002'
        self.run_export(); self.assert_failed(self.bad_key, 'no output', [1])

    def test_address_pass_runs_after_legacy_inference_with_frozen_attributes(self):
        self.stage = 'vs'; self.run_export()
        self.assertEqual([j['constant_buffer_address'] for j in self.job_history], [False, False, True])
        self.assertEqual([j['attr'] for j in self.job_history], [{}, {'0':'uint'}, {'0':'uint'}])
        self.assertEqual(json.loads(self.report.read_text())['failed'], [])

    def test_inputs_not_inferred_still_get_address_optimization(self):
        self.stage = 'vs'; self.inference = {}; self.run_export()
        self.assertEqual([j['constant_buffer_address'] for j in self.job_history], [False, True])
        self.assertTrue(all(j['attr'] == {} for j in self.job_history))

    def test_failed_optimized_pass_cannot_reuse_baseline_product(self):
        self.stage = 'vs'; self.behavior = 'optimized-missing'
        self.second_shader = True; self.bad_key = '0000000000000002'
        self.run_export(); self.assert_failed(self.bad_key, 'no output', [1])

    def test_changed_optimized_input_type_is_rejected(self):
        self.stage = 'vs'; self.behavior = 'input-type-change'
        self.run_export(); self.assert_failed('0000000000000001', 'changed the baseline vertex input interface', [])

    def test_stale_translator_cannot_silently_skip_interface_baseline(self):
        self.stage = 'vs'; self.behavior = 'old-translator'
        self.run_export(); self.assert_failed('0000000000000001', 'did not disable', [])

    def test_export_reports_exact_expected_inventory(self):
        self.run_export()
        self.assertTrue(self.report.is_file())
        report = json.loads(self.report.read_text())
        self.assertEqual(report['expected'], [['0000000000000001', 5]])
        self.assertEqual(report['failed'], [])
        with dmsv.Bundle(str(self.out)) as b:
            self.assertEqual([(x[0], x[1]) for x in b.entries], [(1, 5)])

    def test_opt_in_is_acknowledged_and_not_reinferred_after_optimization(self):
        self.stage = 'vs'
        calls = []
        self.inference = {}
        # Existing orchestration fixtures expose inference through this property.
        class Once(dict):
            def items(inner):
                calls.append(True)
                if len(calls) != 1: raise AssertionError('Inference ran after address sharing')
                return super().items()
        self.inference = Once({'0':'uint'})
        self.run_export()
        self.assertEqual(len(calls), 1)



class ProductValidation(unittest.TestCase):
    def test_vertex_array_shape_uses_length_values_not_spirv_ids(self):
        import shader_products
        a = shader_products.vertex_input_signature(vertex_module(array_length=4))
        self.assertEqual(a, shader_products.vertex_input_signature(vertex_module(array_length=4, id_offset=20)))
        self.assertNotEqual(a, shader_products.vertex_input_signature(vertex_module(array_length=6)))

    def test_vertex_interface_ignores_ids_but_keeps_types_and_locations(self):
        import shader_products
        signature = shader_products.vertex_input_signature(vertex_module())
        self.assertEqual(signature, shader_products.vertex_input_signature(vertex_module(id_offset=20)))
        for change in (dict(numeric='sint'), dict(numeric='uint'), dict(width=16), dict(channels=2), dict(location=10), dict(component=1)):
            with self.subTest(change=change):
                self.assertNotEqual(signature, shader_products.vertex_input_signature(vertex_module(**change)))

    def test_unlocated_non_builtin_input_is_rejected(self):
        import shader_products
        data = vertex_module()
        words = list(struct.unpack(f'<{len(data)//4}I', data)); at = 5
        while at < len(words):
            count, op = words[at]>>16, words[at]&65535
            if op == 71 and words[at+2] == 30: break
            at += count
        removed = words[:at]+words[at+count:]
        with self.assertRaises(ValueError):
            shader_products.vertex_input_signature(struct.pack(f'<{len(removed)}I', *removed))

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

    def test_storage_binding_requires_matching_metadata(self):
        import shader_products
        binds = [dict(name='cp_s0', binding=3, type='ssbo')]
        res = reflection()
        with self.assertRaises(ValueError): shader_products.validate_bindings(binds, res)
        row = dict(binding=3, slot=0, cb_slot=0, cb_offset=132, flags=1)
        res['sbufs'] = [row]
        shader_products.validate_bindings(binds, res)
        res['sbufs'] = [{**row, 'binding': 4}]
        with self.assertRaises(ValueError): shader_products.validate_bindings(binds, res)

    def test_batch_exit_protocol_requires_complete_matching_summary(self):
        import shader_products
        for code, summary in [(1, ''), (1, 'translated: 0 ok, 2 failed'),
                              (0, 'translated: 0 ok, 1 failed'),
                              (1, 'translated: 1 ok, 0 failed'),
                              (1, 'translated: 0 ok, 1 failed\ncrashed')]:
            with self.subTest(code=code, summary=summary), self.assertRaises(RuntimeError):
                shader_products.check_batch(subprocess.CompletedProcess([], code, summary, ''), 1)
        shader_products.check_batch(subprocess.CompletedProcess([], 1, 'translated: 0 ok, 1 failed\n', ''), 1)

    def test_bundle_inventory_checked_without_reading_payloads_in_both_versions(self):
        import shader_products
        report = {'schema': 1, 'expected': [['0000000000000001', 5]], 'failed': []}
        with tempfile.TemporaryDirectory(prefix='nx2a_shader_products_') as tmp:
            path = Path(tmp) / 'out.dmsv'
            for level in ([0, 3] if dmsv.zstd_mod() else [0]):
                dmsv.write(str(path), [(1, 5, module('cs'), struct.pack('<5I', 0, 0, 1, 1, 1))], level=level)
                with mock.patch.object(dmsv.Bundle, 'spv', side_effect=AssertionError('duplicate module read')), \
                     mock.patch.object(dmsv.Bundle, 'refl', side_effect=AssertionError('duplicate reflection read')):
                    self.assertEqual(shader_products.check_bundle(path, report), (1, 1, True))
                for bad in ({'schema':1,'expected':[['0000000000000002',5]],'failed':[]},
                            {'schema':1,'expected':[['0000000000000001',5]]*2,'failed':[]},
                            {'schema':1,'expected':[['0000000000000001',5]],'failed':[['0000000000000001','missing']]}):
                    with self.assertRaises(ValueError): shader_products.check_bundle(path, bad)
                partial = dict(schema=1, expected=[['0000000000000001',5], ['0000000000000002',5]],
                               failed=[['0000000000000002', 'no output']])
                self.assertEqual(shader_products.check_bundle(path, partial), (1, 2, False))
                dmsv.write(str(path), [], level=level)
                empty = dict(schema=1, expected=[['0000000000000001',5]], failed=[['0000000000000001', 'no output']])
                self.assertEqual(shader_products.check_bundle(path, empty), (0, 1, False))


if __name__ == '__main__': unittest.main()
