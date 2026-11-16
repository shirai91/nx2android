"""An empty compute variant must not lose real shaders in the same Unity asset."""
import importlib.util
import os
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/gfx/tools'


@unittest.skipUnless(importlib.util.find_spec('lz4'), 'no lz4')
class EmptyCompute(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(TOOLS))
        import unityfs
        cls.unityfs = unityfs
        spec = importlib.util.spec_from_file_location('split_empty_compute', TOOLS / 'shader_split.py')
        cls.s = importlib.util.module_from_spec(spec)
        with patch.dict(os.environ, {'DM_GAME': '0100000000010000'}): spec.loader.exec_module(cls.s)
        ctl = bytearray(0x880); ctl[:4] = cls.s.CTL_MAGIC
        code = cls.s.CS_MAGIC + bytes(0xfc)
        struct.pack_into('<I', ctl, 0x20, len(ctl))
        struct.pack_into('<I', ctl, 0x704, len(code))
        header = [0] * 24
        header[6:12] = [-1] * 5 + [0]
        header[17], header[23] = len(ctl), len(code)
        cls.ctl, cls.code = bytes(ctl), code
        cls.program = struct.pack('<24i', *header) + ctl + code

    def extract(self, kernels):
        obj = {'m_Name': 'Particles', 'variants': [{'kernels': kernels}]}
        with patch.object(self.unityfs, 'serialized_objects', return_value=[(72, 1, obj)]):
            return self.s.unity_compute(b'', '/resources.assets', {})

    def test_empty_variants_keep_surrounding_programs_and_indices(self):
        for layout in ('uniqueVariants', 'variantMap', 'direct'):
            with self.subTest(layout=layout):
                variants = [{'code': self.program}, {'code': b''}, {'code': self.program}]
                if layout == 'direct':
                    kernels = [dict(v, name='Kernel' + str(i)) for i, v in enumerate(variants)]
                    names = ['Kernel0.0', 'Kernel2.0']
                else:
                    wrapped = variants if layout == 'uniqueVariants' else [{'first': str(i), 'second': v} for i, v in enumerate(variants)]
                    kernels = [{'name': 'Kernel', layout: wrapped}]
                    names = ['Kernel.0', 'Kernel.2']
                self.assertEqual(self.extract(kernels), [('/resources.assets#Particles#' + n, 'cs', self.ctl, self.code) for n in names])

    def test_all_empty_produces_no_program(self):
        self.assertEqual(self.extract([{'name': 'Empty', 'uniqueVariants': [{'code': b''}]}]), [])

    def test_nonempty_truncated_program_still_fails(self):
        with self.assertRaises(struct.error):
            self.extract([{'name': 'Broken', 'uniqueVariants': [{'code': b'bad'}]}])


if __name__ == '__main__': unittest.main()
