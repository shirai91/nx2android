"""The production job selects the existing donor Vulkan offset policy."""
import ast, os, struct, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class TextureTarget(unittest.TestCase):
    def test_production_compute_job_selects_vulkan_offsets(self):
        source = ROOT / 'nxnative/dm/android/tools/shader_vk.py'
        nodes = [n for n in ast.parse(source.read_text()).body if isinstance(n, ast.FunctionDef) and n.name == 'job_for']
        env = dict(os=os, struct=struct, SB_ALIGN=256, SUBGROUP=128, GATHER_BIAS=0, FACTS={})
        exec(compile(ast.Module(body=nodes, type_ignores=[]), str(source), 'exec'), env)
        record = bytearray(0x900); control = 0x80
        record[control:control+4] = bytes.fromhex('34127698')
        struct.pack_into('<4I', record, control + 0x748, 1, 1, 1, 0)
        with tempfile.TemporaryDirectory() as work:
            job, constants = env['job_for']('0123456789abcdef', 'cs', bytes(record), work)
            self.assertEqual(job.get('host_target'), 'vulkan')
            self.assertEqual(job['local'], [1, 1, 1])
