"""Translate a synthetic Maxwell EXIT shader and inspect its actual SPIR-V interface."""
import json, shutil, struct, subprocess, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('dotnet'), 'the shader translator needs .NET')
class PerVertexInterface(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='nx2a_spirv_')
        cls.out = Path(cls.tmp.name)
        src = ROOT / 'nxnative/dm/gfx/tools/dmsh_xlate'
        subprocess.run(['dotnet', 'build', str(src), '-c', 'Release', '-o', str(cls.out / 'xlate'), '-nologo', '-v', 'quiet'], check=True, capture_output=True)
        # SPH: vertex stage. One scheduler word, then an unconditional EXIT (P7).
        code = bytearray(0x80)
        struct.pack_into('<I', code, 0, 0x461)
        struct.pack_into('<Q', code, 0x58, 0xe30000000007000f)
        binary = cls.out / 'exit.bin'; binary.write_bytes(code)
        job = dict(key='exit', stage='vs', code=str(binary), tex={}, img={}, attr={}, fout={}, debug=False, reduced=True)
        jobs = cls.out / 'jobs.json'; jobs.write_text(json.dumps([job]), encoding='utf-8')
        subprocess.run(['dotnet', str(cls.out / 'xlate/dmsh_xlate.dll'), str(jobs), str(cls.out)], check=True, capture_output=True)
        data = (cls.out / 'exit.spv').read_bytes()
        words = struct.unpack(f'<{len(data) // 4}I', data)
        cls.instructions = []
        i = 5
        while i < len(words):
            n, op = words[i] >> 16, words[i] & 0xffff
            if not n: raise AssertionError('zero-length SPIR-V instruction')
            cls.instructions.append((op, words[i + 1:i + n])); i += n

    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()

    def test_unused_cull_distance_is_not_declared(self):
        # A CullDistance array never written by this shader is not part of its interface.
        cull = [args for op, args in self.instructions if op == 72 and args[2:] == (11, 4)]
        self.assertEqual(cull, [], 'unused gl_CullDistance consumes a device limit and has no defined output')

    def test_clip_and_cull_interface_fits_vulkan_minimum(self):
        constants = {a[1]: a[2] for op, a in self.instructions if op == 43 and len(a) == 3}
        arrays = {a[0]: constants[a[2]] for op, a in self.instructions if op == 28}
        structs = {a[0]: a[1:] for op, a in self.instructions if op == 30}
        totals = {}
        for op, a in self.instructions:
            if op == 72 and a[2] == 11 and a[3] in (3, 4):
                totals[a[0]] = totals.get(a[0], 0) + arrays[structs[a[0]][a[1]]]
        self.assertTrue(all(n <= 8 for n in totals.values()), f'clip + cull array sizes: {totals}')
        self.assertEqual(sum(totals.values()), 0, 'an EXIT-only shader must not expose unused clipping planes')

    def mask_shader(self, mask, native, attributes=0xffff):
        # A passthrough GS writes one primitive's viewport mask, with a raw output bound of one.
        code = bytearray(0x80)
        struct.pack_into('<I', code, 0, 0x61 | (4 << 10) | (1 << 24))
        struct.pack_into('<I', code, 12, 7 << 24)   # triangle strip
        struct.pack_into('<I', code, 16, 1)
        mov = int('000000010000' + '0' * 52, 2) | (7 << 16) | (15 << 12) | (mask << 20)
        ast = int('1110111111110' + '0' * 51, 2) | (255 << 8) | (255 << 39) | (7 << 16) | (0x3a0 << 20)
        struct.pack_into('<3Q', code, 0x58, mov, ast, 0xe30000000007000f)
        key = f'mask-{mask}-{native}-{attributes}'
        binary = self.out / f'{key}.bin'; binary.write_bytes(code)
        job = dict(key=key, stage='gs', code=str(binary), tex={}, img={}, attr={}, fout={}, debug=False, reduced=True,
                   gs_passthrough=False, gs_input=3, viewport_mask=native, gs_passthrough_mask=attributes)
        jobs = self.out / f'{key}.json'; jobs.write_text(json.dumps([job]), encoding='utf-8')
        subprocess.run(['dotnet', str(self.out / 'xlate/dmsh_xlate.dll'), str(jobs), str(self.out)], check=True, capture_output=True)
        data = (self.out / f'{key}.spv').read_bytes(); words = struct.unpack(f'<{len(data) // 4}I', data)
        result = []; i = 5
        while i < len(words):
            n, op = words[i] >> 16, words[i] & 0xffff
            self.assertGreater(n, 0)
            result.append((op, words[i + 1:i + n])); i += n
        return result

    def test_single_viewport_fallback_has_no_nvidia_capability(self):
        for mask in (0, 1):
            with self.subTest(mask=mask):
                instructions = self.mask_shader(mask, False)
                self.assertFalse((17, (5255,)) in instructions, 'ShaderViewportMaskNV is unsupported')
                self.assertFalse(any(op == 72 and a[2:] == (11, 3) for op, a in instructions),
                                 'software passthrough must not declare unused clip planes on both interfaces')
                self.assertTrue(any(op == 250 for op, _ in instructions), 'mask must control whether the primitive is emitted')
                self.assertTrue(any(op == 59 and a[2] == 6 for op, a in instructions), 'mask must be private shader state')
                private = {a[1] for op, a in instructions if op == 59 and a[2] == 6}
                pointers = private | {a[1] for op, a in instructions if op == 65 and a[2] in private}
                constants = {a[1]: a[2] for op, a in instructions if op == 43 and len(a) == 3}
                for op, a in instructions:
                    if op == 124 and a[2] in constants: constants[a[1]] = constants[a[2]]   # OpBitcast
                stored = [constants.get(a[1]) for op, a in instructions if op == 62 and a[0] in pointers]
                self.assertIn(mask, stored, 'the guest mask store must survive; an uninitialized predicate loses culling')

    def test_native_viewport_mask_keeps_its_capability(self):
        self.assertIn((17, (5255,)), self.mask_shader(1, True))

    def test_geometry_forwards_only_consumer_locations(self):
        instructions = self.mask_shader(1, False, 0xffe)
        locations = {a[0]: a[2] for op, a in instructions if op == 71 and a[1] == 30}
        inputs = {a[1] for op, a in instructions if op == 59 and a[2] == 1}
        self.assertEqual({locations[v] for v in inputs if v in locations}, set(range(1, 12)))

    def test_written_clip_distance_is_preserved(self):
        code = bytearray(0x80)
        struct.pack_into('<I', code, 0, 0x461)
        # AST gl_ClipDistance[0] = RZ, followed by EXIT.
        ast = int('1110111111110' + '0' * 51, 2) | (255 << 8) | (255 << 39) | (7 << 16) | (0x2c0 << 20)
        struct.pack_into('<2Q', code, 0x58, ast, 0xe30000000007000f)
        binary = self.out / 'clip.bin'; binary.write_bytes(code)
        job = dict(key='clip', stage='vs', code=str(binary), tex={}, img={}, attr={}, fout={}, debug=False, reduced=True)
        jobs = self.out / 'clip.json'; jobs.write_text(json.dumps([job]), encoding='utf-8')
        subprocess.run(['dotnet', str(self.out / 'xlate/dmsh_xlate.dll'), str(jobs), str(self.out)], check=True, capture_output=True)
        data = (self.out / 'clip.spv').read_bytes(); words = struct.unpack(f'<{len(data) // 4}I', data)
        decorated = []; i = 5
        while i < len(words):
            n, op = words[i] >> 16, words[i] & 0xffff
            self.assertGreater(n, 0)
            if op == 72: decorated.append(words[i + 1:i + n])
            i += n
        self.assertTrue(any(a[2:] == (11, 3) for a in decorated), 'actual clip stores require the ClipDistance member')


if __name__ == '__main__': unittest.main()
