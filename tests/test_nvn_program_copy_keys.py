"""A copy of an NVNprogram made after SetShaders finds the stage keys SetShaders computed (hle_nvn.c pk_rec, found by prog_t.serial), and the table of them is bounded.

Unity copies an NVNprogram after SetShaders and draws with the copy (Ori and the Will of the Wisps 1.2.1): looked up by the program's address, the copy had no
record, so its stages were keyed at the first draw from control sections Unity had freed. On a POCO X3 Pro the run logged 142 "[RED] shader ... is not in the
bundle" keys, none of them in the game's 53,678 Unity shader stages, skipped the draws into the window texture and showed a black screen with sound.

A record is not dropped at Finalize (a copy can outlive the program it came from), so the table holds at most PK_CAP records and a new one takes the place of
the program bound least recently; a stage keyed late holds a host copy of its control section only until its first bind.

The harness compiles hle_nvn.c's program functions as they are (keys_hash, prog_key, the pk_* table, nvnProgramSetShaders, nvnProgramFinalize), with a
stand-in hash in place of CommonCrypto and LOG on stdout. It runs where tests/host_cc.py's posix() builds (macOS and Linux; the Windows runner's MinGW
is skipped, as for the other NVN harnesses), and checks: a copy gets the key of the original; a stage whose code was in no pool at SetShaders is keyed later
from the host's copy of its control section, not from the game's freed one, and that copy is freed then; Finalize does not drop the keys a copy still needs;
a new SetShaders gives a new record; many SetShaders keep the table at PK_CAP records, drop the program bound least recently and never one bound often, free
a dropped record's control copies, and the counter line logs the live counts.
"""
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

import csrc
from host_cc import posix

COUNTER = re.compile(r'\[nvn\] program keys: (\d+) SetShaders so far, (\d+) records kept \(at most (\d+)\), (\d+) control section copies waiting for their '
                     r'code \((\d+) KB at most\), (\d+) records dropped, (\d+) bound again after \(frame (\d+)\)')

HARNESS_HEAD = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_obj.h"

#define NVN(name, ret, ...) static ret h_##name(__VA_ARGS__)
#define LOG(...) (printf(__VA_ARGS__), putchar('\n'))   // on stdout, where the test reads the counter lines
#define OLOG(...) ((void)0)
uint64_t dm_nvn_frame_count(void) { return 7; }
uint64_t nvn_new_serial(void) { static uint64_t n; return ++n; }
static int g_released;
void mtl_prog_release(prog_t *p) { (void)p; g_released++; }
// the pool the code areas lie in: [g_pool, g_pool + g_pool_len); 0 = none registered yet
static uintptr_t g_pool; static size_t g_pool_len;
static int mtl_addr(uint64_t a, uint64_t n, void **mb, uint64_t *mo) { *mb = NULL; *mo = 0; return g_pool && a >= g_pool && a + n <= g_pool + g_pool_len; }

// stand-in for CommonCrypto's SHA-256: FNV-1a over the bytes, spread over the digest (the key only has to follow the bytes)
#define CC_SHA256_DIGEST_LENGTH 32
typedef struct { uint64_t h; } CC_SHA256_CTX;
static void CC_SHA256_Init(CC_SHA256_CTX *c) { c->h = 1469598103934665603ull; }
static void CC_SHA256_Update(CC_SHA256_CTX *c, const void *d, size_t n) { const uint8_t *b = d; for (size_t i = 0; i < n; i++) c->h = (c->h ^ b[i]) * 1099511628211ull; }
static void CC_SHA256_Final(uint8_t *md, CC_SHA256_CTX *c) { for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) md[i] = (uint8_t)(c->h >> (8 * (i & 7)) ^ (uint64_t)i); }

'''

HARNESS_TAIL = r'''
enum { NCTL = 0x880, NCODE = 0x100 };
static uint8_t *control(uint8_t seed) {   // an NVN control section: +0x20 its own length (0x880), +0x704 the code area's length
    uint8_t *c = malloc(NCTL); for (int i = 0; i < NCTL; i++) c[i] = (uint8_t)(seed * 31 + i * 7);
    uint32_t own = NCTL, n = NCODE; memcpy(c + 0x20, &own, 4); memcpy(c + 0x704, &n, 4); return c;
}
static uint64_t key_of(const prog_t *p, unsigned st) { uint8_t k[8]; assert(prog_key(p, st, k)); uint64_t v; memcpy(&v, k, 8); return v; }
static uint64_t want_key(const uint8_t *ctl, const uint8_t *code) {   // the key of a live control section and code area, as the shader split computes it
    NVNshaderData sd = {(uint64_t)(uintptr_t)code, ctl}; uint8_t k[8]; assert(keys_hash(&sd, 0, k)); uint64_t v; memcpy(&v, k, 8); return v;
}
static void scribble(uint8_t *c) { memset(c, 0xa5, NCTL); free(c); }   // the game frees the control section once SetShaders returns

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    static uint8_t code[2][NCODE]; for (int i = 0; i < NCODE; i++) { code[0][i] = (uint8_t)i; code[1][i] = (uint8_t)(255 - i); }
    if (!strcmp(argv[1], "copy_after_set_shaders")) {   // code in a pool at SetShaders: keyed there; the copy carries the serial
        g_pool = (uintptr_t)code; g_pool_len = sizeof code;
        NVNprogram a, b; h_nvnProgramInitialize(&a, NULL);
        uint8_t *c = control(1); uint64_t want = want_key(c, code[0]);
        NVNshaderData sd = {(uint64_t)(uintptr_t)code[0], c}; assert(h_nvnProgramSetShaders(&a, 1, &sd));
        scribble(c);
        memcpy(&b, &a, sizeof a);
        assert(key_of((prog_t *)&b, 0) == want);
        assert(key_of((prog_t *)&a, 0) == want);
        return 0;
    }
    if (!strcmp(argv[1], "keyed_later_from_host_copy")) {   // code in no pool at SetShaders (registered before the first draw): keyed at the draw from the host's copy
        g_pool = 0;
        NVNprogram a, b; h_nvnProgramInitialize(&a, NULL);
        uint8_t *c = control(2); uint64_t want = want_key(c, code[1]);
        NVNshaderData sd = {(uint64_t)(uintptr_t)code[1], c}; assert(h_nvnProgramSetShaders(&a, 1, &sd));
        scribble(c);
        g_pool = (uintptr_t)code; g_pool_len = sizeof code;
        memcpy(&b, &a, sizeof a);
        assert(key_of((prog_t *)&b, 0) == want);
        assert(key_of((prog_t *)&a, 0) == want);
        return 0;
    }
    if (!strcmp(argv[1], "finalize_keeps_copy_keys")) {   // the game finalizes the program it copied from; the copy still draws
        g_pool = (uintptr_t)code; g_pool_len = sizeof code;
        NVNprogram a, b; h_nvnProgramInitialize(&a, NULL);
        uint8_t *c = control(3); uint64_t want = want_key(c, code[0]);
        NVNshaderData sd = {(uint64_t)(uintptr_t)code[0], c}; assert(h_nvnProgramSetShaders(&a, 1, &sd));
        scribble(c);
        memcpy(&b, &a, sizeof a);
        h_nvnProgramFinalize(&a);
        assert(key_of((prog_t *)&b, 0) == want);
        return 0;
    }
    if (!strcmp(argv[1], "set_again_new_record")) {   // SetShaders again on the same program: the new shaders' keys, not the old record's
        g_pool = (uintptr_t)code; g_pool_len = sizeof code;
        NVNprogram a; h_nvnProgramInitialize(&a, NULL);
        uint8_t *c1 = control(4), *c2 = control(5); uint64_t w1 = want_key(c1, code[0]), w2 = want_key(c2, code[1]); assert(w1 != w2);
        NVNshaderData s1 = {(uint64_t)(uintptr_t)code[0], c1}; assert(h_nvnProgramSetShaders(&a, 1, &s1)); scribble(c1);
        assert(key_of((prog_t *)&a, 0) == w1);
        NVNshaderData s2 = {(uint64_t)(uintptr_t)code[1], c2}; assert(h_nvnProgramSetShaders(&a, 1, &s2)); scribble(c2);
        assert(key_of((prog_t *)&a, 0) == w2);
        return 0;
    }
    if (!strcmp(argv[1], "late_copy_freed_when_keyed")) {   // a stage keyed late holds a host copy of its control section until its first bind, then frees it
        g_pool = 0;
        NVNprogram a; h_nvnProgramInitialize(&a, NULL);
        uint8_t *c0 = control(6), *c1 = control(7); uint64_t w0 = want_key(c0, code[0]), w1 = want_key(c1, code[1]);
        NVNshaderData sd[2] = {{(uint64_t)(uintptr_t)code[0], c0}, {(uint64_t)(uintptr_t)code[1], c1}}; assert(h_nvnProgramSetShaders(&a, 2, sd));
        scribble(c0); scribble(c1);
        assert(g_pk_nctl == 2);
        g_pool = (uintptr_t)code; g_pool_len = sizeof code;
        assert(key_of((prog_t *)&a, 0) == w0 && g_pk_nctl == 1);
        assert(key_of((prog_t *)&a, 1) == w1 && g_pk_nctl == 0);
        assert(key_of((prog_t *)&a, 1) == w1 && key_of((prog_t *)&a, 0) == w0);   // kept in the record
        return 0;
    }
    if (!strcmp(argv[1], "bounded")) {   // SetShaders 3 * PK_CAP times with distinct programs: the table stays at PK_CAP records
        g_pool = (uintptr_t)code; g_pool_len = sizeof code;
        NVNprogram hot, cold, late, tmp;
        uint8_t *hot_c = control(8); uint64_t hot_w = want_key(hot_c, code[0]);   // serial 1, drawn now and then: never dropped
        h_nvnProgramInitialize(&hot, NULL); NVNshaderData hs = {(uint64_t)(uintptr_t)code[0], hot_c}; assert(h_nvnProgramSetShaders(&hot, 1, &hs)); scribble(hot_c);
        uint8_t *cold_c = control(9); uint64_t cold_w = want_key(cold_c, code[1]);   // serial 2, never drawn: the first to go; its control section stays the game's
        h_nvnProgramInitialize(&cold, NULL); NVNshaderData cs = {(uint64_t)(uintptr_t)code[1], cold_c}; assert(h_nvnProgramSetShaders(&cold, 1, &cs));
        g_pool = 0; uint8_t *late_c = control(10);   // serial 3, keyed late and never drawn: its control copy goes with its record
        h_nvnProgramInitialize(&late, NULL); NVNshaderData ls = {(uint64_t)(uintptr_t)code[0], late_c}; assert(h_nvnProgramSetShaders(&late, 1, &ls)); scribble(late_c);
        assert(g_pk_nctl == 1);
        g_pool = (uintptr_t)code;
        uint8_t *c = control(11); NVNshaderData ts = {(uint64_t)(uintptr_t)code[0], c};
        const int total = 3 * PK_CAP;
        for (int i = 0; i < total; i++) {
            h_nvnProgramInitialize(&tmp, NULL); assert(h_nvnProgramSetShaders(&tmp, 1, &ts)); h_nvnProgramFinalize(&tmp);
            if (i % 1000 == 0) assert(key_of((prog_t *)&hot, 0) == hot_w);
            assert(g_pk_n <= PK_CAP);
        }
        assert(g_pk_n == PK_CAP && g_pk_sets == (unsigned)total + 3 && g_pk_dropped == (unsigned)total + 3 - PK_CAP);
        assert(g_pk_nctl == 0);
        assert(key_of((prog_t *)&hot, 0) == hot_w && g_pk_again == 0);
        assert(key_of((prog_t *)&cold, 0) == cold_w && g_pk_again == 1);   // its record went: keyed again from its own control section, the game's
        assert(g_pk_n == PK_CAP && g_pk_dropped == (unsigned)total + 4 - PK_CAP);
        free(c); free(cold_c);
        return 0;
    }
    return 2;
}
'''


@unittest.skipUnless(posix(), 'a POSIX host C compiler is required')
class NvnProgramCopyKeysTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        src = csrc.read('nxnative/dm/gfx/nvn/hle_nvn.c')
        cut = csrc.between(src, '// ---------- Programs ----------', '// ---------- Sync / events ----------', 'hle_nvn.c')
        cls._tmp = tempfile.TemporaryDirectory(prefix='nx2a_prog_keys_')
        tmp = Path(cls._tmp.name)
        c, cls.exe = tmp / 'check.c', tmp / 'check'
        c.write_text(HARNESS_HEAD + cut + HARNESS_TAIL, encoding='utf-8')
        nvn_dir = csrc.REPO / 'nxnative/dm/gfx/nvn'
        res = subprocess.run(
            ['cc', '-std=gnu11', '-O1', '-Wall', '-Wno-unused-function', '-Werror', f'-I{nvn_dir}', f'-I{nvn_dir / "include"}', str(c), '-pthread', '-o', str(cls.exe)],
            capture_output=True, text=True,
        )
        if res.returncode != 0:
            cls._tmp.cleanup()
            raise AssertionError(res.stdout + res.stderr)

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, '_tmp'):
            cls._tmp.cleanup()

    def run_case(self, case):
        res = subprocess.run([str(self.exe), case], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        return res.stdout

    def test_scenarios(self):
        for case in ('copy_after_set_shaders', 'keyed_later_from_host_copy', 'finalize_keeps_copy_keys', 'set_again_new_record', 'late_copy_freed_when_keyed'):
            with self.subTest(case=case):
                self.run_case(case)

    def test_table_bounded_and_counted(self):
        out = self.run_case('bounded')
        cap = 16384
        lines = COUNTER.findall(out)
        # 3 programs set before the loop (serials 1-3), then 3 * cap: a line at the first SetShaders and every 1024th
        self.assertEqual([int(m[0]) for m in lines], [1] + list(range(1024, 3 * cap + 3, 1024)))
        for t, kept, most, copies, kb, dropped, again, frame in ((int(x) for x in m) for m in lines):
            with self.subTest(t=t):
                self.assertEqual((most, kept, dropped, again, frame), (cap, min(t, cap), max(0, t - cap), 0, 7))
                self.assertEqual(copies, 1 if 3 <= t and t - cap < 2 else 0)   # serial 3's copy, until its record goes (the second one dropped)
                self.assertEqual(kb, copies * 0x880 // 1024)
        self.assertIn(f'[nvn] program key table full ({cap} records): a new record took the place of the program bound least recently (serial 2; '
                      '1 records dropped so far, frame 7)', out)
        self.assertIn('(serial 1025; 1024 records dropped so far, frame 7)', out)   # and every 1024th: serials 2, 3, 4, ... in turn, never serial 1, which is drawn
        self.assertIn('[nvn] program serial 2 was bound with no key record (dropped when the table was full): keyed again from its own control sections, '
                      'which the game may have freed (1 times so far, frame 7)', out)


if __name__ == '__main__':
    unittest.main()
