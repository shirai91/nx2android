# porter install's seeding (porter/install.py seed_from_sibling): a new install copies the RomFS / DLC on the device from another port of the same game version
# whose pack has the same content key (porter/datapack.py part_key), instead of pushing them again; a copy that differs is deleted and pushed as before.
import hashlib, json, re, shlex, sys, tempfile, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
from porter import datapack, install   # noqa: E402

A = '/sdcard/Android/data/org.example.game/files'
B = '/sdcard/Android/data/org.example.game.test/files'


class FakeAdb:
    """Files on the device as {path: bytes}; the few shell commands seeding runs"""
    def __init__(self, fs): self.fs = dict(fs); self.cmds = []

    def shell(self, cmd, **kw):
        self.cmds.append(cmd)
        if cmd.startswith('ls -d /sdcard/Android/data/*/files/nx2a-data.json'):
            return '\n'.join(sorted(p for p in self.fs if re.fullmatch(r'/sdcard/Android/data/[^/]+/files/nx2a-data.json', p)))
        if m := re.fullmatch(r'mkdir -p (\S+) && cp -r (\S+) (\S+)', cmd):
            src, dst = shlex.split(m.group(2))[0], shlex.split(m.group(3))[0].rstrip('/')
            part = src.rsplit('/', 1)[1]
            for p, v in list(self.fs.items()):
                if p.startswith(src + '/'): self.fs[f'{dst}/{part}/{p[len(src) + 1:]}'] = v
            return ''
        if m := re.fullmatch(r'rm -rf (\S+)', cmd):
            d = shlex.split(m.group(1))[0]
            for p in [p for p in self.fs if p.startswith(d + '/')]: del self.fs[p]
            return ''
        raise AssertionError(f'unexpected shell: {cmd}')

    def read_text(self, path): return self.fs.get(path, b'').decode('utf-8')
    def remote_sizes(self, d): return {p[len(d) + 1:]: len(v) for p, v in self.fs.items() if p.startswith(d + '/')}
    def remote_md5(self, d, rels, **kw): return {r: hashlib.md5(self.fs[f'{d}/{r}']).hexdigest() for r in rels if f'{d}/{r}' in self.fs}


def pack(t, files):
    for rel, data in files.items():
        p = Path(t) / rel; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(data)
    return {'title_id': '0100000000AAA000', 'package': 'org.example.game.test', 'parts': {p: k for p in datapack.SEEDABLE if (k := datapack.part_key(t, p))}}


class Seed(unittest.TestCase):
    FILES = {'RomFS/a.bin': b'a' * 10, 'RomFS/d/b.bin': b'b' * 20, 'aoc/1/c.bin': b'c' * 5}

    def device(self, man, sibling_files, sibling_title='0100000000AAA000'):
        fs = {f'{A}/{rel}': data for rel, data in sibling_files.items()}
        fs[f'{A}/nx2a-data.json'] = json.dumps(dict(man, title_id=sibling_title, package='org.example.game')).encode()
        return FakeAdb(fs)

    def test_copies_matching_parts(self):
        with tempfile.TemporaryDirectory() as t:
            man = pack(t, self.FILES); adb = self.device(man, self.FILES)
            self.assertEqual(install.seed_from_sibling(adb, man, B, {}, t), ['RomFS', 'aoc'])
            self.assertEqual(adb.remote_sizes(f'{B}/RomFS'), {'a.bin': 10, 'd/b.bin': 20})
            self.assertEqual(adb.remote_sizes(f'{B}/aoc'), {'1/c.bin': 5})

    def test_another_game_or_version_is_not_used(self):
        with tempfile.TemporaryDirectory() as t:
            man = pack(t, self.FILES)
            self.assertEqual(install.seed_from_sibling(self.device(man, self.FILES, '0100000000BBB000'), man, B, {}, t), [])
            other = dict(man, parts=dict(man['parts'], RomFS=dict(man['parts']['RomFS'], key='0' * 64)))   # another RomFS, the same DLC
            adb = self.device(other, self.FILES); adb.fs[f'{A}/nx2a-data.json'] = json.dumps(dict(other, package='org.example.game')).encode()
            self.assertEqual(install.seed_from_sibling(adb, man, B, {}, t), ['aoc'])

    def test_a_copy_that_differs_is_deleted(self):
        with tempfile.TemporaryDirectory() as t:
            man = pack(t, self.FILES); bad = dict(self.FILES, **{'RomFS/a.bin': b'x' * 10})   # same size, other content
            adb = self.device(man, bad)
            self.assertEqual(install.seed_from_sibling(adb, man, B, {}, t), ['aoc'])
            self.assertEqual(adb.remote_sizes(f'{B}/RomFS'), {})

    def test_a_part_already_there_is_left_to_the_push(self):
        with tempfile.TemporaryDirectory() as t:
            man = pack(t, self.FILES); adb = self.device(man, self.FILES)
            self.assertEqual(install.seed_from_sibling(adb, man, B, {'RomFS/a.bin': 10}, t), ['aoc'])

    def test_pack_manifest_has_content_keys(self):
        with tempfile.TemporaryDirectory() as t:
            k1 = pack(t, self.FILES)['parts']
        with tempfile.TemporaryDirectory() as t:
            k2 = pack(t, dict(self.FILES, **{'RomFS/a.bin': b'a' * 11}))['parts']
        self.assertEqual(k1['RomFS']['files'], 2); self.assertEqual(k1['aoc'], k2['aoc']); self.assertNotEqual(k1['RomFS']['key'], k2['RomFS']['key'])


if __name__ == '__main__':
    unittest.main()
