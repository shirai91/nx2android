"""The existing IoStore shader reader must be reachable without a companion pak."""
import json
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from porter.upstream import profile_guess, uepak
from porter.pipeline import Port
from porter.upstream import BuildError


def main_nso(path):
    image = bytearray(0x200)
    struct.pack_into('<II', image, 0, 0x14000004, 0x60)
    struct.pack_into('<I', image, 0x10, 0xd503201f)
    image[0x60:0x64] = b'MOD0'
    struct.pack_into('<i', image, 0x64, 0x40)
    struct.pack_into('<ii', image, 0x70, 0x20, 0x2c)
    image[0x80:0x84] = bytes((1, 3, 3, 3))
    struct.pack_into('<qQqQ', image, 0xa0, 6, 0x100, 5, 0x130)
    struct.pack_into('<I', image, 0x118, 1)
    image[0x131:0x144] = b'nvnBootstrapLoader\0'
    header = bytearray(0x100); header[:4] = b'NSO0'
    struct.pack_into('<III4xIII4xIII', header, 0x10, 0x100, 0, 0x40, 0x140, 0x40, 0x60, 0x1a0, 0xa0, 0x160)
    header[0x40:0x50] = bytes(range(16))
    struct.pack_into('<III', header, 0x60, 0x40, 0x60, 0x160)
    path.write_bytes(header + image)


def container(path, library=True, version=4):
    shader = b'artificial shader payload'
    library_id = b'library0000' + bytes([uepak.IO_SHADER_LIBRARY if library else 1])
    shader_id = b'shader00000' + bytes([uepak.IO_SHADER_CODE])
    n = len(shader)
    data = b''.join([struct.pack('<III', 1, 0, 1), bytes(20), struct.pack('<I', 1), shader_id,
                     struct.pack('<IIQI', 0, 1, 0, 1), struct.pack('<IIIIII', 0, 1, n, n, 1, 0)])
    header = bytearray(144); header[:16] = uepak.TOC_MAGIC; header[16] = version
    struct.pack_into('<9I', header, 20, 144, 2, 1, 12, 0, 0, 65536, 0, 1)
    ranges = b''.join(o.to_bytes(5, 'big') + size.to_bytes(5, 'big') for o, size in ((0, len(data)), (len(data), n)))
    size = (len(data) + n).to_bytes(3, 'little')
    path.with_suffix('.utoc').write_bytes(header + library_id + shader_id + ranges + bytes(5) + size * 2 + bytes(1))
    path.with_suffix('.ucas').write_bytes(data + shader)
    return shader


class IoStoreDetection(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='nx2a_iostore_detection_'); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name); self.exefs = self.root / 'ExeFS'; self.romfs = self.root / 'RomFS'
        self.exefs.mkdir(); self.romfs.mkdir(); main_nso(self.exefs / 'main')

    def test_existing_reader_is_detected_without_a_pak(self):
        shader = container(self.romfs / 'Synthetic')
        src = profile_guess.build_sources(self.exefs, self.romfs, ['main'])
        self.assertEqual(src['graphics'][0], 'nvn')
        self.assertTrue(src['ue_decode'][0], src['ue_decode'][1])
        self.assertIn('1 IoStore', src['ue_decode'][1])
        work = self.root / 'decoded'
        def no_oodle(*args): self.fail('stored fixture must not invoke Oodle')
        listing, count = uepak.job(str(self.romfs), str(self.exefs / 'main'), str(work), decode=no_oodle)
        self.assertEqual((len(listing), listing[0]['shaders'], count), (1, 1, 0))
        output = (work / 'archives' / listing[0]['name']).read_bytes()
        parsed = uepak.archive(output)
        self.assertEqual(output[parsed['code']:], shader)
        self.assertEqual(json.loads((work / 'job.json').read_text(encoding='utf-8'))['records'], [])

    def test_nonshader_and_unsupported_containers_do_not_request_decode(self):
        container(self.romfs / 'NoLibrary', library=False)
        container(self.romfs / 'OldNumbering', version=3)
        (self.romfs / 'Invalid.utoc').write_bytes(b'not an IoStore TOC')
        self.assertFalse(profile_guess.build_sources(self.exefs, self.romfs, ['main'])['ue_decode'][0])

    def test_port_selects_existing_device_decode_and_checks_device_requirement(self):
        container(self.romfs / 'Synthetic')
        port = SimpleNamespace(game=self.root, g=SimpleNamespace(words=lambda _: ['main']), app_name='Synthetic',
                               tools={}, o=SimpleNamespace(device=None), cfg={}, dirs={'v': self.root}, versions=['v'])
        with self.assertRaisesRegex(BuildError, 'needs adb'):
            Port.check_graphics(port)
        port.tools['adb'] = 'synthetic-adb'
        with patch('porter.pipeline.Adb') as adb:
            Port.check_graphics(port)
            adb.return_value.pick.assert_called_once()
        self.assertIn('1 IoStore', port.ue_why)

    def test_truncated_tocs_do_not_abort_shader_source_detection(self):
        container(self.romfs / 'Truncated')
        toc = self.romfs / 'Truncated.utoc'
        complete = toc.read_bytes()
        for length in (len(uepak.TOC_MAGIC), 144, 160, len(complete) - 1):
            with self.subTest(length=length):
                toc.write_bytes(complete[:length])
                self.assertFalse(profile_guess.build_sources(self.exefs, self.romfs, ['main'])['ue_decode'][0])

    def test_graphics_classification_still_excludes_gl_and_vulkan(self):
        container(self.romfs / 'Synthetic')
        for imports, names, want in [(['vkCreateInstance'], 0, 'vulkan'), (['eglCreateWindowSurface'], 1, 'gl')]:
            with self.subTest(graphics=want), patch.object(profile_guess, 'nso', return_value=(b'glShaderSource' * names, 0, 0)), patch.object(profile_guess.nso_syms, 'dynsyms', return_value=[(n, False, 0) for n in imports]):
                src = profile_guess.build_sources(self.exefs, self.romfs, ['main'])
                self.assertEqual(src['graphics'][0], want)
                self.assertFalse(src['ue_decode'][0])


if __name__ == '__main__': unittest.main()
