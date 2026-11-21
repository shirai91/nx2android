"""Copied donor implicit/bias texture-offset fixture and preservation assertions.

Source: nxnative ea6dcf3f7, pinned 1f01e28c87da5233dfa14fb31fabf1535df33013.
Only translator invocation and target pass imports differ from donor tests.
"""
import json, os, shutil, struct, subprocess, tempfile, unittest
from pathlib import Path
from shader_gather_fixture import base, handle, mov, shader, SLOT_128, SAMPLER_1
from spv_module import decode, opcode
import spv_gather, spv_half, spv_precise, spv_texture_offsets

def program(bias):
    def load(reg, offset):
        return base('Ldc') | (4 << 48) | (3 << 36) | (offset << 20) | (7 << 16) | (255 << 8) | reg
    def ipa(reg, offset):
        return base('Ipa') | (offset << 28) | (7 << 16) | reg
    def f2i(reg, source):
        return base('F2i') | (3 << 39) | (source << 20) | (7 << 16) | (2 << 10) | (2 << 8) | reg
    def multiply(reg, a, b):
        return base('Fmul') | (b << 20) | (7 << 16) | (a << 8) | reg
    header = bytearray(0x50)
    struct.pack_into('<I', header, 0, (5 << 10) | (3 << 5) | 1)
    struct.pack_into('<I', header, 72, 15)  # Fragment RGBA output in R0..R3.
    # FragCoord supplies every signed-four-bit pair over a 16x16 target.
    # Coordinates use the original continuous position, never the offsets.
    ops = handle(6, SLOT_128, SAMPLER_1) + [ipa(0, 0x70), ipa(1, 0x74),
        f2i(12, 0), f2i(13, 1), mov(14, 4),
        base('Shl') | (14 << 20) | (7 << 16) | (13 << 8) | 13,
        base('Lop') | (1 << 41) | (13 << 20) | (7 << 16) | (12 << 8) | (8 if bias else 7)]
    if bias: ops += [load(7, 8)]
    ops += [load(10, 0), load(11, 4), multiply(0, 0, 10), multiply(1, 1, 11),
            base('TexB') | ((2 if bias else 0) << 37) | (1 << 36) | (15 << 31) |
            (2 << 28) | (6 << 20) | (7 << 16)]
    return bytes(header) + shader(ops)

@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE') and shutil.which('spirv-val'),
                     'requires DMSH_TEST_XLATE and SPIRV-Tools')
class FragmentOffsets(unittest.TestCase):
    def test_implicit_and_bias_samples_keep_the_original_derivative_context(self):
        with tempfile.TemporaryDirectory(prefix='fragment_offsets_') as directory:
            work = Path(directory); jobs = []
            for target in ('metal', 'vulkan'):
                for bias in (False, True):
                    key = target + str(int(bias)); binary = work / (key + '.bin')
                    binary.write_bytes(program(bias))
                    jobs.append(dict(key=key, stage='ps', code=str(binary), host_target=target,
                                     tex={'128': '2d'}, fout={'0': 'float'}))
            path = work / 'jobs.json'; path.write_text(json.dumps(jobs))
            subprocess.run(['dotnet', os.environ['DMSH_TEST_XLATE'], str(path), str(work)], check=True)
            for job in jobs:
                with self.subTest(key=job['key']):
                    bias = job['key'].endswith('1')
                    raw = (work / (job['key'] + '.spv')).read_bytes()
                    samples = [i for i in decode(raw)[1] if opcode(i) == 87]
                    self.assertEqual(len(samples), 1)
                    self.assertEqual(samples[0][5], 0x10 | int(bias))
                    data = spv_gather.lower(spv_precise.lower(spv_half.lower(spv_texture_offsets.lower(raw))))
                    exported = decode(data)[1]
                    samples = [i for i in exported if opcode(i) == 87]
                    self.assertEqual(len(samples), 256)
                    self.assertTrue(all(i[5] == 8 | int(bias) for i in samples))
                    self.assertFalse(any(opcode(i) in (105, 250, 251) for i in exported),
                                     'no LOD query or varying branch may replace implicit derivatives')
                    valid = subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=data, capture_output=True)
                    self.assertEqual(valid.returncode, 0, valid.stderr.decode())
