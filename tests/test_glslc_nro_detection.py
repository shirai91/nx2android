"""Runtime GLSL source detection inside .nro modules and AOC/NRO staging in device_glslc."""
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from porter.pipeline import Port
from porter.upstream import profile_guess


class GlslcNroDetectionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='nx2a_glslc_nro_')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.exefs = self.root / 'ExeFS'
        self.romfs = self.root / 'RomFS'
        self.exefs.mkdir()
        self.romfs.mkdir()
        (self.exefs / 'main').write_bytes(b'MAIN')
        (self.exefs / 'subsdk0').write_bytes(b'SUB0')

    def _sources(self):
        def fake_nso(path):
            return (b'MAIN' if Path(path).name == 'main' else b'SUB0', '00' * 16, [4, 0, 0], 0)

        def fake_dynsyms(img):
            if img == b'MAIN':
                return [('nvnBootstrapLoader', False, 0), ('glslcCompile', False, 0)]
            return [('glslcCompile', True, 1)]

        with patch.object(profile_guess, 'nso', side_effect=fake_nso), \
             patch.object(profile_guess.nso_syms, 'dynsyms', side_effect=fake_dynsyms):
            return profile_guess.build_sources(self.exefs, self.romfs, ['main', 'subsdk0'])

    def test_detects_glsl_sources_inside_nro_when_main_imports_glslc_compile(self):
        nro_dir = self.romfs / '.nro'
        nro_dir.mkdir()
        (nro_dir / 'game.nro').write_bytes(b'NRO0\0#version 430\nvoid main() {}\0')
        src = self._sources()
        self.assertEqual(src['graphics'][0], 'nvn')
        self.assertTrue(src['glslc_capture'][0], src['glslc_capture'][1])

    def test_no_glsl_in_main_or_nro_does_not_request_capture(self):
        nro_dir = self.romfs / '.nro'
        nro_dir.mkdir()
        (nro_dir / 'empty.nro').write_bytes(b'NRO0\0no shaders here\0')
        src = self._sources()
        self.assertFalse(src['glslc_capture'][0])

    def test_device_glslc_mounts_aoc_and_never_truncates_nro_files(self):
        ver_dir = self.root / 'extract/v0'
        rom = ver_dir / 'RomFS'
        (rom / '.nro').mkdir(parents=True)
        (rom / '.nro/game.nro').write_bytes(b'N' * 20)
        (rom / 'big.dat').write_bytes(b'D' * 20)

        aoc_dir = self.root / 'aoc'
        (aoc_dir / '1').mkdir(parents=True)
        (aoc_dir / '1/small.vs').write_bytes(b'VS')
        (aoc_dir / '1/big.bin').write_bytes(b'A' * 20)

        work = self.root / 'work'
        aw = work / 'android'
        logs = work / 'logs'
        patched = work / 'patched'
        for d in (aw, logs, patched):
            d.mkdir(parents=True)
        shmap = self.root / 'shader_map.tsv'
        shmap.write_text('# empty\n', encoding='utf-8')
        cap_name = ('0' * 16) + '.glslc'

        for has_aoc in (True, False):
            pushed = []
            shells = []

            class FakeAdb:
                def __init__(self, *a, **kw):
                    pass

                def pick(self):
                    return 'serial'

                def shell(self, cmd, **kw):
                    shells.append(cmd)
                    if './dmhost mods' in cmd:
                        return 'NX2A_GLSLC_EXIT=0\n'
                    return ''

                def push_file(self, src, dst, **kw):
                    pushed.append(('file', Path(src).name, dst))

                def push_tree(self, src, dst, **kw):
                    rels = sorted(p.relative_to(src).as_posix() for p in Path(src).rglob('*') if p.is_file())
                    pushed.append(('tree', dst, rels))

                def run(self, op, src, dst, **kw):
                    if op == 'pull' and str(src).endswith('/capture'):
                        Path(dst).mkdir(parents=True, exist_ok=True)
                        (Path(dst) / cap_name).write_bytes(b'glslc')
                    elif op == 'pull' and str(src).endswith('/run.log'):
                        Path(dst).write_text('ok\n', encoding='utf-8')
                    return ''

            cap_dir = ver_dir / 'glslc_capture'
            if cap_dir.exists():
                for f in cap_dir.iterdir():
                    f.unlink()
                cap_dir.rmdir()

            port = SimpleNamespace(
                dirs={'v0': ver_dir},
                versions=['v0'],
                pkg_ver='v0',
                g=SimpleNamespace(id='0100000000000000', table=lambda n: shmap),
                tools={'adb': 'adb', 'ndk': 'ndk'},
                o=SimpleNamespace(device='', debug=False, jobs=1),
                cfg={},
                app_name='TestApp',
                glslc_why='test',
                aw=aw,
                logs=logs,
                work=work,
                patched=patched,
                env={},
                aoc=aoc_dir if has_aoc else None,
                GLSLC_SECS=Port.GLSLC_SECS,
                GLSLC_SMALL=10,
                glslc_todo=lambda: (None, ['v0']),
            )

            with self.subTest(has_aoc=has_aoc):
                with patch('porter.pipeline.Adb', FakeAdb), \
                     patch('porter.pipeline.nsp.glslc_done', return_value={cap_name}), \
                     patch('porter.pipeline.nsp.run'):
                    Port.device_glslc(port)

                rom_push = [p for p in pushed if p[0] == 'tree' and p[1].endswith('/RomFS')]
                self.assertEqual(len(rom_push), 1)
                self.assertIn('.nro/game.nro', rom_push[0][2])
                self.assertNotIn('big.dat', rom_push[0][2])

                dmhost_cmd = next(c for c in shells if './dmhost mods' in c)
                if has_aoc:
                    self.assertIn('DM_AOC_DIR=/data/local/tmp/nx2a-glslc-', dmhost_cmd)
                    aoc_push = [p for p in pushed if p[0] == 'tree' and p[1].endswith('/aoc')]
                    self.assertEqual(len(aoc_push), 1)
                    self.assertIn('1/small.vs', aoc_push[0][2])
                    self.assertNotIn('1/big.bin', aoc_push[0][2])
                    self.assertTrue(any('truncate -s 20' in c and '/aoc/1/big.bin' in c for c in shells))
                else:
                    self.assertNotIn('DM_AOC_DIR=', dmhost_cmd)
                    self.assertFalse(any(p[0] == 'tree' and p[1].endswith('/aoc') for p in pushed))


if __name__ == '__main__':
    unittest.main()
