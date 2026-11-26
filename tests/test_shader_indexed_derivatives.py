"""Translate synthetic Maxwell quad derivatives through the real shader pipeline."""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from test_shader_perspective_cancellation import require_dotnet10

ROOT = Path(__file__).resolve().parents[1]


def shuffle(dest, source, lane, mask=0x1c03, predicate=7):
    return (0xef10000000000000 | (predicate << 48) | (mask << 34) |
            (3 << 28) | (lane << 20) | (7 << 16) | (source << 8) | dest)


def shader(lane, *, origin_lane=0, origin_mask=0x1c03, final_mask=0x1c03,
           swizzle=0xd7, different_input=False, vertex=False, predicate=False):
    code = bytearray(0x100)
    struct.pack_into('<I', code, 0, 0x61 | ((1 if vertex else 5) << 10))
    struct.pack_into('<I', code, 72, 1)  # fragment target 0, red component
    # ALD R2, position.x; a varying input, not a constant folded by the optimizer.
    load = 0xefd8000000000000 | (255 << 39) | (0x70 << 20) | (7 << 16) | (255 << 8) | 2
    ops = [load, shuffle(3, 2, origin_lane, origin_mask),
           0x50f8000000000000 | (swizzle << 28) | ((255 if different_input else 2) << 20) |
           (7 << 16) | (3 << 8) | 4,
           shuffle(0, 4, lane, final_mask, 0 if predicate else 7)]
    if predicate:
        # @!P0 MOV32I R0, 1.0. The indexed shuffle's valid predicate is always true.
        ops.append(0x0100000000000000 | (0x3f800000 << 20) | (15 << 12) | (1 << 19))
    if vertex:
        ops.append(0xeff0000000000000 | (255 << 39) | (0x70 << 20) | (7 << 16) | (255 << 8))
    ops.append(0xe30000000007000f)
    offset = 0x50
    for index, op in enumerate(ops):
        if index % 3 == 0:
            offset += 8  # Maxwell scheduler word
        struct.pack_into('<Q', code, offset, op)
        offset += 8
    return code


def instructions(data):
    words = struct.unpack(f'<{len(data) // 4}I', data)
    offset = 5
    while offset < len(words):
        size, op = words[offset] >> 16, words[offset] & 0xffff
        if size == 0 or offset + size > len(words):
            raise AssertionError('invalid SPIR-V instruction framing')
        yield op, words[offset + 1:offset + size]
        offset += size


class IndexedDerivatives(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dotnet = os.environ.get('DOTNET') or shutil.which('dotnet')
        require_dotnet10(cls.dotnet)
        cls.tmp = tempfile.TemporaryDirectory(prefix='nx2a_quad_')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.out = Path(cls.tmp.name).resolve()
        project = ROOT / 'nxnative/dm/gfx/tools/dmsh_xlate/dmsh_xlate.csproj'
        result = subprocess.run([cls.dotnet, 'build', str(project), '-c', 'Release',
                                 '--artifacts-path', str(cls.out / 'artifacts'),
                                 '-p:NuGetAudit=false', '--nologo', '-v:q'],
                                capture_output=True, text=True, timeout=180)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        cls.dll = cls.out / 'artifacts/bin/dmsh_xlate/release/dmsh_xlate.dll'

    def translate(self, lane, **kwargs):
        binary = self.out / 'quad.bin'
        binary.write_bytes(shader(lane, **kwargs))
        jobs = self.out / 'jobs.json'
        jobs.write_text(json.dumps([dict(key='quad', stage='vs' if kwargs.get('vertex') else 'fs',
                                         code=str(binary), tex={}, img={}, attr={}, fout={},
                                         debug=False, reduced=True)]), encoding='utf-8')
        result = subprocess.run([self.dotnet, str(self.dll), str(jobs), str(self.out)],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = (self.out / 'quad.spv').read_bytes()
        validator = os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')
        if validator:
            checked = subprocess.run([validator, '--target-env', 'vulkan1.1', str(self.out / 'quad.spv')],
                                     capture_output=True, text=True, timeout=30)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        return list(instructions(data))

    def test_indexed_x_and_y_emit_coarse_derivatives_without_subgroups(self):
        for lane, expected in ((1, 213), (2, 214)):  # OpDPdxCoarse / OpDPdyCoarse
            with self.subTest(lane=lane):
                code = self.translate(lane)
                self.assertEqual([op for op, _ in code if op in (213, 214)], [expected])
                self.assertIn((17, (51,)), code)  # Capability DerivativeControl
                self.assertFalse(any(333 <= op <= 366 for op, _ in code), 'matched pattern still uses subgroups')
                self.assertFalse(any(op == 71 and args[1:] == (11, 41) for op, args in code), 'lane ID survived')

    def test_valid_predicate_is_preserved(self):
        code = self.translate(1, predicate=True)
        self.assertEqual(sum(op == 213 for op, _ in code), 1)
        # @!P0 stays a branch, on the negation of the constant-true valid predicate, so it is never taken.
        true_ids = {args[1] for op, args in code if op == 41}  # OpConstantTrue
        negated = {args[1]: args[2] for op, args in code if op == 168}  # OpLogicalNot
        branches = [args[0] for op, args in code if op == 250]  # OpBranchConditional
        self.assertEqual(len(branches), 1)
        self.assertIn(negated.get(branches[0]), true_ids, 'constant-true valid predicate was lost')

    def test_near_matches_keep_shuffle_semantics(self):
        for kwargs in ({'origin_lane': 1}, {'origin_mask': 0x1f}, {'final_mask': 0x1f},
                       {'swizzle': 0x99}, {'different_input': True}):
            with self.subTest(**kwargs):
                code = self.translate(1, **kwargs)
                self.assertFalse(any(op in (213, 214) for op, _ in code))
                self.assertTrue(any(op == 345 for op, _ in code), 'unmatched shuffle was removed')

    def test_other_lanes_are_not_derivatives(self):
        for lane in (0, 3):
            with self.subTest(lane=lane):
                self.assertFalse(any(op in (213, 214) for op, _ in self.translate(lane)))

    def test_vertex_stage_keeps_shuffle(self):
        code = self.translate(1, vertex=True)
        self.assertFalse(any(op in (213, 214) for op, _ in code))
        self.assertTrue(any(op == 345 for op, _ in code))
