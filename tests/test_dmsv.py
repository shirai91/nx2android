# The Vulkan shader bundle (nxnative/dm/android/tools/dmsv.py): tiny bundles written, read back, converted between versions 1 and 2, and read by the host's own
# reader (gfx_vk_shader.c, its bundle functions compiled with the vendored zstd decoder when a C compiler is at hand).
import contextlib, io, os, random, re, shutil, struct, subprocess, sys, tempfile, threading, unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'nxnative/dm/android/tools'))
import dmsv   # noqa: E402

ZSTD = dmsv.zstd_mod() is not None


def spirv(seed, n):   # a SPIR-V-like module: the header, then n OpDecorate Location words (repetitive, as the translator's output is: zstd shrinks it)
    rnd = random.Random(seed)
    return struct.pack('<5I', 0x07230203, 0x10000, 0, 100, 0) + b''.join(struct.pack('<4I', 4 << 16 | 71, rnd.randrange(60), 30, rnd.randrange(8)) for _ in range(n))


def refl(nbind, c1):   # shader_vk.py refl_bin's layout: {nbind, c1_len, local[3]}, nbind 20-byte bindings, c1
    return struct.pack('<5I', nbind, len(c1), 1, 1, 1) + bytes(20 * nbind) + c1


ENTRIES = [(0x1327af7c2b78070e, 0, spirv(1, 400), refl(2, b'c1 constants')), (0x094f4afb9c876459, 1, spirv(2, 900), refl(0, b'')),
           (0xfedcba9876543210, 5, struct.pack('<6I', 0x07230203, 0x10000, 0, 100, 0, (256 << 16) | 2) + random.Random(3).randbytes(1019) + b'\0', refl(1, b''))]   # framed OpSourceContinued with random bytes, which zstd cannot shrink; not a semantic shader


def v1_reference(entries):   # the bundle as shader_vk.py wrote it before version 2 (2026-11)
    entries = sorted(entries, key=lambda e: e[0]); hdr = struct.pack('<4sII', b'DMSV', 1, len(entries)); off = len(hdr) + 32 * len(entries); table, blobs = [], []
    for key, st, spv, r in entries:
        table.append(struct.pack('<QIIIIII', key, st, off, len(spv), off + len(spv), len(r), 0)); blobs += [spv, r]; off += len(spv) + len(r)
    return hdr + b''.join(table) + b''.join(blobs)


class Bundle(unittest.TestCase):
    def setUp(self): self.d = Path(tempfile.mkdtemp(prefix='nx2a_test_dmsv_'))
    def tearDown(self): shutil.rmtree(self.d, ignore_errors=True)

    def read(self, p):
        with dmsv.Bundle(str(p)) as b: return b.version, [(e[0], e[1], bytes(b.spv(e)), bytes(b.refl(e))) for e in b.entries], list(b.entries)

    def test_version1_is_the_old_layout(self):   # --zstd 0: byte for byte what the old writer made, and it reads back
        p = self.d / 'v1.dmsv'; self.assertEqual(dmsv.write(str(p), ENTRIES, level=0)[0], 1)
        self.assertEqual(p.read_bytes(), v1_reference(ENTRIES))
        ver, got, _ = self.read(p); self.assertEqual((ver, got), (1, sorted(ENTRIES)))

    @unittest.skipUnless(ZSTD, 'no zstd in this Python')
    def test_version2_round_trip(self):
        p = self.d / 'v2.dmsv'; ver, raw, stored = dmsv.write(str(p), ENTRIES, jobs=2)
        self.assertEqual((ver, raw), (2, sum(len(e[2]) for e in ENTRIES))); self.assertLess(stored, raw)
        ver, got, ents = self.read(p); self.assertEqual((ver, got), (2, sorted(ENTRIES)))
        by = {e[0]: e for e in ents}
        for key, _, spv, _ in ENTRIES[:2]:   # compressed: spv_size = the SPIR-V's length, a zstd frame stored, with its content size and checksum
            e = by[key]; self.assertEqual(e[6], len(spv)); self.assertLess(e[3], len(spv))
            frame = p.read_bytes()[e[2]:e[2] + e[3]]; self.assertEqual(frame[:4], b'\x28\xb5\x2f\xfd'); self.assertTrue(frame[4] & 0x04, 'no checksum flag')
        e = by[ENTRIES[2][0]]; self.assertEqual((e[6], e[3]), (0, len(ENTRIES[2][2])))   # one zstd does not shrink: stored as it is
        self.assertEqual([e[0] for e in ents], sorted(e[0] for e in ENTRIES))

    def test_without_zstd_version1(self):   # a Python without compression.zstd or backports.zstd: version 1, and a [RED] line says why
        p, out = self.d / 'nz.dmsv', io.StringIO()
        with mock.patch.object(dmsv, 'zstd_mod', return_value=None), contextlib.redirect_stdout(out): ver = dmsv.write(str(p), ENTRIES)[0]
        self.assertEqual(ver, 1); self.assertIn('[RED] ', out.getvalue()); self.assertEqual(p.read_bytes(), v1_reference(ENTRIES))

    @unittest.skipUnless(ZSTD, 'no zstd in this Python')
    def test_pack_both_ways(self):   # dmsv.py pack: version 1 → 2 → 1 gives the first file back
        v1, v2, back = self.d / 'a.dmsv', self.d / 'b.dmsv', self.d / 'c.dmsv'; v1.write_bytes(v1_reference(ENTRIES))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(dmsv.main(['pack', str(v1), str(v2), '--level', '3']), 0); self.assertEqual(dmsv.main(['pack', str(v2), str(back), '--level', '0']), 0)
        self.assertEqual(self.read(v2)[0], 2); self.assertEqual(back.read_bytes(), v1.read_bytes()); self.assertFalse(Path(str(back) + '.partial').exists())

    def test_refused(self):   # not a bundle, a version this reader does not know, a copy cut short
        good = v1_reference(ENTRIES)
        for name, data in (('magic', b'NOPE' + good[4:]), ('version', good[:4] + struct.pack('<I', 3) + good[8:]), ('short', good[:-10]), ('table', good[:40])):
            p = self.d / f'{name}.dmsv'; p.write_bytes(data)
            with self.assertRaises(ValueError, msg=name): dmsv.Bundle(str(p))


class Publication(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory(prefix='nx2a_dmsv_publish_')
        self.addCleanup(self.folder.cleanup)
        self.root = Path(self.folder.name)
        self.path = self.root / 'shaders.dmsv'
        self.old = v1_reference(ENTRIES[:1])

    def assert_preserved(self, existed):
        self.assertEqual(self.path.exists(), existed)
        if existed: self.assertEqual(self.path.read_bytes(), self.old)
        self.assertEqual(set(self.root.iterdir()), {self.path} if existed else set())

    def test_interrupted_write_preserves_old_bundle_and_removes_scratch(self):
        real_open, real_fdopen = open, os.fdopen
        class Interrupted:
            def __init__(self, file): self.file, self.calls = file, 0
            def __getattr__(self, name): return getattr(self.file, name)
            def __enter__(self): return self
            def __exit__(self, *exc): self.file.close()
            def write(self, data):
                self.calls += 1
                if self.calls == 2: raise OSError('injected disk write failure')
                return self.file.write(data)
        def wrap(factory):
            def opened(*args, **kwargs):
                file = factory(*args, **kwargs)
                return Interrupted(file) if 'w' in (args[1] if len(args)>1 else kwargs.get('mode','r')) else file
            return opened
        for existed in (True, False):
            with self.subTest(existed=existed):
                if existed: self.path.write_bytes(self.old)
                else: self.path.unlink(missing_ok=True)
                with mock.patch.object(dmsv, 'open', wrap(real_open), create=True), mock.patch.object(dmsv.os, 'fdopen', wrap(real_fdopen)):
                    with self.assertRaisesRegex(OSError, 'injected disk write failure'):
                        dmsv.write(self.path, ENTRIES, level=0)
                self.assert_preserved(existed)

    def test_sync_and_replace_failure_preserve_old_bundle(self):
        for operation in ('fsync', 'replace'):
            with self.subTest(operation=operation):
                self.path.write_bytes(self.old)
                with mock.patch.object(dmsv.os, operation, side_effect=OSError('injected '+operation)):
                    with self.assertRaisesRegex(OSError, 'injected '+operation):
                        dmsv.write(self.path, ENTRIES, level=0)
                self.assert_preserved(True)

    def test_readback_mismatch_cannot_publish(self):
        for existed in (True, False):
            with self.subTest(existed=existed):
                if existed: self.path.write_bytes(self.old)
                else: self.path.unlink(missing_ok=True)
                with mock.patch.object(dmsv.Bundle, 'spv', return_value=b'corrupted'):
                    with self.assertRaises(ValueError): dmsv.write(self.path, ENTRIES, level=0)
                self.assert_preserved(existed)

    def test_readback_open_failure_closes_handles_before_cleanup(self):
        real_open, real_fstat = open, os.fstat
        class FailedMap(dmsv.mmap.mmap):
            def __new__(cls, *args, **kwargs): raise OSError('injected mmap failure')
        for operation in ('fstat', 'mmap'):
            opened = []
            def track(*args, **kwargs):
                file = real_open(*args, **kwargs); opened.append(file); return file
            def failed_stat(fd):
                if any(not file.closed and file.mode == 'rb' and file.fileno() == fd for file in opened):
                    raise OSError('injected fstat failure')
                return real_fstat(fd)
            failure = (mock.patch.object(dmsv.os, 'fstat', side_effect=failed_stat)
                       if operation == 'fstat' else mock.patch.object(dmsv.mmap, 'mmap', FailedMap))
            self.path.write_bytes(self.old)
            with self.subTest(operation=operation), mock.patch.object(dmsv, 'open', track, create=True), failure:
                try: dmsv.write(self.path, ENTRIES, level=0)
                except OSError as error:
                    self.assertIn('injected '+operation, str(error))
                    # Keep the exception live: its traceback retains a failed
                    # constructor, and Windows cannot delete its open file.
                    self.assertTrue(all(file.closed for file in opened))
                else: self.fail('injected readback failure was ignored')
            self.assert_preserved(True)

    def test_concurrent_writers_use_distinct_scratch_files(self):
        replace = os.replace; barrier = threading.Barrier(2); scratch = []
        def rendezvous(source, destination):
            scratch.append(source); barrier.wait(timeout=10); replace(source, destination)
        self.path.write_bytes(self.old)
        with mock.patch.object(dmsv.os, 'replace', side_effect=rendezvous), ThreadPoolExecutor(2) as pool:
            jobs = [pool.submit(dmsv.write, self.path, entries, level=0) for entries in (ENTRIES[:1], ENTRIES)]
            for job in jobs: job.result(timeout=15)
        self.assertEqual(len(set(scratch)), 2)
        self.assertIn(self.path.read_bytes(), (self.old, v1_reference(ENTRIES)))
        self.assertEqual(set(self.root.iterdir()), {self.path})

    @unittest.skipIf(os.name == 'nt', 'POSIX permission bits')
    def test_replacement_keeps_destination_permissions(self):
        self.path.write_bytes(self.old); self.path.chmod(0o640)
        dmsv.write(self.path, ENTRIES, level=0)
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o640)
        self.assertEqual(self.path.read_bytes(), v1_reference(ENTRIES))


@unittest.skipUnless(shutil.which('cc') and ZSTD, 'a host C compiler and zstd in this Python are needed')
class HostReader(unittest.TestCase):   # native bundle reader and vk_shader_q through reflection, before Vulkan module creation
    @classmethod
    def setUpClass(cls):
        cls.d = Path(tempfile.mkdtemp(prefix='nx2a_test_dmsv_c_')); src = (REPO / 'nxnative/dm/android/gfx_vk_shader.c').read_text(encoding='utf-8')
        body = src[src.index('// spv_size: 0 ='):src.index('// Run-time capture:')]
        lookup_start = src.index('static vkshader_t *vk_shader_q(uint64_t key, int stage, int quiet) {')
        lookup = src[lookup_start:src.index("    // SPIR-V with the stage's set", lookup_start)]
        header = (REPO / 'nxnative/dm/android/gfx_vk.h').read_text(encoding='utf-8')
        types = header[header.index('typedef struct __attribute__((packed))'):header.index('} vkshader_t;') + len('} vkshader_t;')]
        set_start = src.index('static int set_of(')
        set_of = src[set_start:src.index('\n', set_start)]
        harness = r'''
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "zstd.h"
#include "nvn_hmap.h"
#define VKLOG(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
// Guard all pages after the index before vk_shaders_open can touch them.
// Fixtures place both SPIR-V and reflection entirely on those pages.
static void *index_only_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    void *p = mmap(addr, len, prot, flags, fd, off);
    if (p != MAP_FAILED && getenv("DMSV_PROTECT_PAYLOAD")) {
        uint32_t n; memcpy(&n, (uint8_t *)p + 8, 4);
        size_t page = (size_t)sysconf(_SC_PAGESIZE), end = (12 + 32ull * n + page - 1) / page * page;
        if (end >= len || mprotect((uint8_t *)p + end, len - end, PROT_NONE)) exit(4);
    }
    return p;
}
#define mmap index_only_mmap
''' + body + r'''
#undef mmap
typedef void *VkShaderModule, *VkDescriptorSetLayout, *VkBuffer;
typedef uint64_t VkDeviceSize;
''' + types + '\n' + set_of + r'''
static nvn_hmap g_shaders = {.klen = 16};
static pthread_mutex_t g_sh_mu = PTHREAD_MUTEX_INITIALIZER;
''' + lookup + r'''
    // The production path above validates and consumes reflection. Stop before
    // Vulkan rewriting/module creation, retaining its SPIR-V for the assertions.
    s->spv = w; s->nspv = nw;
    nvn_hm_put(&g_shaders, &hk, s); pthread_mutex_unlock(&g_sh_mu); return s;
}
int main(int argc, char **argv) {   // each shader: key, stage, words, FNV-1a 64 of its SPIR-V; exit 2 when the bundle is refused
    (void)argc; setenv("DM_VK_SHADERS", argv[1], 1); bundle_auto(); if (!g_bundle) return 2;
    if (getenv("DMSV_PROTECT_PAYLOAD")) { printf("%u shaders indexed\n", g_nent); return 0; }
    for (uint32_t i = 0; i < g_nent; i++) {
        uint64_t key = g_ent[i].key; if (find(key) != &g_ent[i]) return 3;
        vkshader_t *s = vk_shader_q(key, g_ent[i].stage, 0);
        if (vk_shader_q(key, g_ent[i].stage, 0) != s) return 5;   // cached success or cached absence
        if (!s) { printf("%016llx fail\n", (unsigned long long)key); continue; }
        uint32_t nw = s->nspv, *w = s->spv;
        uint64_t h = 0xcbf29ce484222325ull; const uint8_t *p = (const uint8_t *)w; for (uint64_t k = 0; k < 4ull * nw; k++) h = (h ^ p[k]) * 0x100000001b3ull;
        printf("%016llx %d %u %016llx\n", (unsigned long long)key, vk_shader_stage(key), nw, (unsigned long long)h); free(w);
    }
    return 0;
}
'''
        (cls.d / 'h.c').write_text(harness, encoding='utf-8'); z = REPO / 'nxnative/dm/third_party/zstd'
        subprocess.run(['cc', '-O1', '-std=gnu11', '-Wall', '-Werror', '-Wno-unused-function', '-I', str(z), '-I', str(REPO / 'nxnative/dm/gfx/nvn'), '-c', str(cls.d / 'h.c'), '-o', str(cls.d / 'h.o')], check=True)
        subprocess.run(['cc', '-O1', '-w', '-fvisibility=hidden', '-DZSTDLIB_VISIBLE=', '-DZSTDERRORLIB_VISIBLE=', '-c', str(z / 'zstddeclib.c'), '-o', str(cls.d / 'z.o')], check=True)
        subprocess.run(['cc', str(cls.d / 'h.o'), str(cls.d / 'z.o'), '-o', str(cls.d / 'h')], check=True)

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.d, ignore_errors=True)

    def run_host(self, p, **env): return subprocess.run([str(self.d / 'h'), str(p)], capture_output=True, text=True, timeout=60, env={**os.environ, **env})

    def expect(self, entries):
        def fnv(b):
            h = 0xcbf29ce484222325
            for x in b: h = ((h ^ x) * 0x100000001b3) & 0xffffffffffffffff
            return h
        return ''.join(f'{k:016x} {st} {len(spv) // 4} {fnv(spv):016x}\n' for k, st, spv, _ in sorted(entries))

    def test_both_versions(self):
        for level in (0, 3):
            p = self.d / f'l{level}.dmsv'; dmsv.write(str(p), ENTRIES, level=level, jobs=2); r = self.run_host(p)
            self.assertEqual(r.returncode, 0, r.stderr); self.assertEqual(r.stdout, self.expect(ENTRIES))
            self.assertIn(f'version {1 if level == 0 else 2}, 3 shaders ({0 if level == 0 else 2} zstd-compressed)', r.stderr)

    def test_damage(self):   # a frame whose bytes changed fails its checksum: that shader alone is left out, with a [RED] line
        p = self.d / 'bad.dmsv'; dmsv.write(str(p), ENTRIES, jobs=2)
        with dmsv.Bundle(str(p)) as b: e = next(x for x in b.entries if x[6])
        data = bytearray(p.read_bytes()); data[e[2] + e[3] // 2] ^= 0x55; p.write_bytes(bytes(data)); r = self.run_host(p)
        self.assertEqual(r.returncode, 0, r.stderr); self.assertIn(f'{e[0]:016x} fail', r.stdout); self.assertIn('[RED] ', r.stderr); self.assertEqual(r.stdout.count('fail'), 1)

    def test_invalid_index_contract(self):
        good = v1_reference(ENTRIES)
        fields = list(dmsv.ENT.unpack_from(good, dmsv.HDR.size))
        mutations = [('stage', 1, 6), ('spv_in_index', 2, 12), ('reflection_in_index', 4, 12),
                     ('v1_compressed', 6, fields[3]), ('short_reflection', 5, 19)]
        for name, field, value in mutations:
            data = bytearray(good); entry = fields.copy(); entry[field] = value
            dmsv.ENT.pack_into(data, dmsv.HDR.size, *entry)
            p = self.d / f'{name}.dmsv'; p.write_bytes(data); r = self.run_host(p)
            self.assertEqual(r.returncode, 2, name + ': ' + r.stdout); self.assertIn('[RED] ', r.stderr)
        for duplicate in (False, True):
            data = bytearray(good)
            first, second = (dmsv.ENT.unpack_from(data, dmsv.HDR.size+i*dmsv.ENT.size) for i in range(2))
            struct.pack_into('<Q', data, dmsv.HDR.size, second[0])
            if not duplicate: struct.pack_into('<Q', data, dmsv.HDR.size+dmsv.ENT.size, first[0])
            p = self.d / f'keys-{duplicate}.dmsv'; p.write_bytes(data); r = self.run_host(p)
            self.assertEqual(r.returncode, 2, r.stdout); self.assertIn('[RED] ', r.stderr)

    def test_open_reads_only_header_and_index(self):
        data = bytearray(v1_reference(ENTRIES))
        table_end = dmsv.HDR.size + len(ENTRIES) * dmsv.ENT.size
        page = os.sysconf('SC_PAGE_SIZE'); padding = (-table_end) % page
        for i in range(len(ENTRIES)):
            offset = dmsv.HDR.size + i * dmsv.ENT.size
            entry = list(dmsv.ENT.unpack_from(data, offset)); entry[2] += padding; entry[4] += padding
            dmsv.ENT.pack_into(data, offset, *entry)
        data[table_end:table_end] = bytes(padding)
        p = self.d / 'protected-payload.dmsv'; p.write_bytes(data)
        r = self.run_host(p, DMSV_PROTECT_PAYLOAD='1')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, '3 shaders indexed\n')

    def test_reflection_counts_reject_only_the_requested_record(self):
        good = v1_reference(ENTRIES)
        first = dmsv.ENT.unpack_from(good, dmsv.HDR.size)
        for bindings, constants in ((0xffffffff, 0), (0, 0xffffffff), (1, 0), (0, 1)):
            # The first record has only a 20-byte reflection header; even a
            # small nonzero count must not leave its declared record.
            with self.subTest(bindings=bindings, constants=constants):
                data = bytearray(good); struct.pack_into('<II', data, first[4], bindings, constants)
                p = self.d / f'counts-{bindings}-{constants}.dmsv'; p.write_bytes(data); r = self.run_host(p)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertEqual(r.stdout, f'{first[0]:016x} fail\n' + self.expect([e for e in ENTRIES if e[0] != first[0]]))
                self.assertIn('[RED] ', r.stderr); self.assertIn('left out as if missing', r.stderr)
                self.assertEqual(r.stderr.count('[RED] '), 1)   # the rejected lookup is cached

    def test_malformed_module_is_rejected_before_rewriting(self):
        original = bytearray(spirv(1, 2))
        for name, word, value in (('zero_word_count', 5, 71), ('instruction_overrun', 5, (20 << 16) | 71),
                                  ('version', 1, 0x20000), ('too_new', 1, 0x10400), ('revision', 1, 0x10301), ('schema', 4, 1)):
            module = original.copy(); struct.pack_into('<I', module, word*4, value)
            entries = [ENTRIES[0], (0x77, 5, bytes(module), refl(0, b''))]
            p = self.d / f'{name}.dmsv'; dmsv.write(p, entries, level=0); r = self.run_host(p)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn('0000000000000077 fail', r.stdout, name)
            self.assertEqual(r.stdout.count('fail'), 1, name); self.assertIn('[RED] ', r.stderr)

    def test_refused(self):   # a version this host does not read, a copy cut short: no bundle (nothing drawn) and a [RED] line, never a read past the file
        good = (self.d / 'g.dmsv'); dmsv.write(str(good), ENTRIES, jobs=2); g = good.read_bytes()
        for name, data in (('version', g[:4] + struct.pack('<I', 3) + g[8:]), ('short', g[:-10]), ('table', g[:40])):
            p = self.d / f'{name}.dmsv'; p.write_bytes(data); r = self.run_host(p)
            self.assertEqual(r.returncode, 2, name); self.assertIn('[RED] ', r.stderr)


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class ReflectionOffset(unittest.TestCase):
    def test_large_bounded_reflection_uses_wide_pointer_offset(self):
        source = (REPO / 'nxnative/dm/android/gfx_vk_shader.c').read_text()
        expression = re.search(r's->c1 = r \+ ([^;]+);', source).group(1)
        code = '''#include <stdint.h>
#include <stddef.h>
#include <assert.h>
static uint64_t offset(int count) {struct {int nbind;} state={count}, *s=&state;return ''' + expression + ''';}
int main(void) {int counts[]={0,1,107374181,107374182,214748363};
for(unsigned i=0;i<sizeof counts/sizeof counts[0];i++)assert(offset(counts[i])==20+20ull*(unsigned)counts[i]);return 0;}
'''
        with tempfile.TemporaryDirectory() as folder:
            src, exe = Path(folder)/'offset.c', Path(folder)/'offset'; src.write_text(code)
            subprocess.run(['cc', '-std=c11', '-fsanitize=undefined', '-fno-sanitize-recover=all', str(src), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
