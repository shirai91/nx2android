"""Full Maxwell size queries retain real dimensions and zero absent ones."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from shader_gather_fixture import base, handle, keep, mov, shader, SLOT_128, SAMPLER_1

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'nxnative/dm/android/tools'))
from spv_module import decode, opcode


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE') and all(shutil.which(t) for t in ('spirv-val', 'spirv-opt')),
                     'requires bundled translator and SPIRV-Tools')
class TextureQueryDimensions(unittest.TestCase):
    def test_all_three_requested_dimensions_follow_the_resolved_texture_shape(self):
        # Keep each query output separately through a real image atomic. This
        # catches the valid-but-wrong scalar case as well as invalid vec2[2].
        query = base('TxqB') | (7 << 31) | (1 << 22) | (6 << 8) | (7 << 16) | 16
        code = shader(handle(6, SLOT_128, SAMPLER_1), mov(7, 0), query,
                      keep(16), keep(17), keep(18))
        shapes = {'1d': 1, '1darray': 2, '2d': 2, 'cube': 2,
                  '2darray': 3, '3d': 3, 'cubearray': 3}
        with tempfile.TemporaryDirectory(prefix='txq-dimensions-') as tmp:
            work = Path(tmp); binary = work / 'query.bin'; binary.write_bytes(code)
            jobs = []
            for target in ('metal', 'vulkan'):
                for shape in shapes:
                    jobs.append(dict(key=target+'-'+shape, stage='cs', code=str(binary),
                                     host_target=target, tex={'128': shape},
                                     img={'0': {'type': '2d', 'fmt': 'R32Uint'}}, local=[1, 1, 1]))
            job_file = work / 'jobs.json'; job_file.write_text(json.dumps(jobs))
            run = subprocess.run(['dotnet', os.environ['DMSH_TEST_XLATE'], job_file, work], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout+run.stderr)
            for job in jobs:
                with self.subTest(key=job['key']):
                    path = work / (job['key']+'.spv')
                    valid = subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', path], capture_output=True, text=True)
                    self.assertEqual(valid.returncode, 0, valid.stderr)
                    # Resolve the translator's register-local loads/stores with
                    # the standard optimizer before inspecting observable writes.
                    optimized = subprocess.run(['spirv-opt', '--ssa-rewrite', '--eliminate-local-single-store',
                                                '--eliminate-local-single-block', '--simplify-instructions',
                                                path, '-o', '-'], capture_output=True, check=True)
                    _, instructions = decode(optimized.stdout)
                    constants = {x[2]: x[3] for x in instructions if opcode(x) == 43}
                    copies = {x[2]: x[3] for x in instructions if opcode(x) in (83, 124)}
                    values = [x[-1] for x in instructions if opcode(x) == 234]
                    self.assertEqual(len(values), 3)
                    for dimension, value in enumerate(values):
                        while value in copies: value = copies[value]
                        if dimension >= shapes[job['tex']['128']]:
                            self.assertEqual(constants.get(value), 0, 'absent dimension must contribute zero')
                        else:
                            self.assertNotIn(value, constants, 'present dimension must read the bound image')


if __name__ == '__main__':
    unittest.main()
