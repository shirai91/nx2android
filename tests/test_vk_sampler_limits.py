"""Donor sampler-limit cases against nx2android's actual creation code."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm'


def production_header():
    source = (DM / 'android/gfx_vk_tex.c').read_text()
    begin = source.index('static VkSamplerAddressMode wrap(')
    end = source.index('\nvolatile int g_vk_samp_log', begin)
    obj = (DM / 'gfx/nvn/nvn_obj.h').read_text()
    params = next(line.split('//')[0] for line in obj.splitlines() if '} samp_par;' in line)
    core = (DM / 'android/gfx_vk_core.c').read_text()
    assignment = re.search(r'V\.max_aniso\s*=.*?;', core).group()
    return params + '\nstatic void init_limits(void) { ' + assignment + ' }\n' + source[begin:end]


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class SamplerLimits(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='nx2a-sampler-')
        cls.addClassCleanup(cls.tmp.cleanup)
        work = Path(cls.tmp.name)
        (work / 'sampler_production.h').write_text(production_header())
        cls.binary = work / 'sampler-limits'
        subprocess.run(['cc', '-std=gnu11', '-O2', '-I' + str(work),
                        '-I' + str(DM / 'third_party/vulkan-headers/include'),
                        str(ROOT / 'tests/vk_sampler_limits.c'), '-lm', '-o', str(cls.binary)],
                       check=True, capture_output=True, text=True)

    def run_mode(self, mode):
        for case in range(9):
            with self.subTest(mode=mode, case=case):
                result = subprocess.run([str(self.binary), str(mode), str(case)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_guest_limits_and_lod_modes(self): self.run_mode(0)
    def test_small_device_limits(self): self.run_mode(1)
    def test_optional_anisotropy_absent(self): self.run_mode(2)


if __name__ == '__main__': unittest.main()
