"""A copy of an NVNprogram made after SetShaders finds the stage keys SetShaders computed (hle_nvn.c pk_get by prog_t.serial).

Unity copies an NVNprogram after SetShaders and draws with the copy (Ori and the Will of the Wisps 1.2.1): looked up by the program's address, the copy had no
record, so its stages were keyed at the first draw from control sections Unity had freed. On a POCO X3 Pro the run logged 142 "[RED] shader ... is not in the
bundle" keys, none of them in the game's 53,678 Unity shader stages, skipped the draws into the window texture and showed a black screen with sound.

The harness compiles hle_nvn.c's program functions as they are (pk_get, keys_hash, prog_key, nvnProgramSetShaders, nvnProgramFinalize), with a stand-in hash and
dictionary so it builds on every CI host, and checks: a copy gets the key of the original; a stage whose code was in no pool at SetShaders is keyed later from
the host's copy of its control section, not from the game's freed one; Finalize does not drop the keys a copy still needs; a new SetShaders gives a new record.
"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import csrc

HARNESS_HEAD = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_obj.h"

#define NVN(name, ret, ...) static ret h_##name(__VA_ARGS__)
#define LOG(...) ((void)0)
#define OLOG(...) ((void)0)
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

// stand-in for CoreFoundation's pointer-keyed dictionary
typedef struct { const void *k[4096]; const void *v[4096]; int n; } dict_t;
typedef dict_t *CFMutableDictionaryRef;
static CFMutableDictionaryRef CFDictionaryCreateMutable(void *a, int n, void *kc, void *vc) { (void)a; (void)n; (void)kc; (void)vc; return calloc(1, sizeof(dict_t)); }
static const void *CFDictionaryGetValue(CFMutableDictionaryRef d, const void *k) { for (int i = 0; i < d->n; i++) if (d->k[i] == k) return d->v[i]; return NULL; }
static void CFDictionarySetValue(CFMutableDictionaryRef d, const void *k, const void *v) {
    for (int i = 0; i < d->n; i++) if (d->k[i] == k) { d->v[i] = v; return; }
    assert(d->n < 4096); d->k[d->n] = k; d->v[d->n] = v; d->n++;
}
static void CFDictionaryRemoveValue(CFMutableDictionaryRef d, const void *k) { for (int i = 0; i < d->n; i++) if (d->k[i] == k) { d->n--; d->k[i] = d->k[d->n]; d->v[i] = d->v[d->n]; return; } }   // (the address-keyed table before the fix)
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
    return 2;
}
'''


class NvnProgramCopyKeysTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
        if not cc:
            raise unittest.SkipTest('a C compiler is needed for the program key check')
        src = csrc.read('nxnative/dm/gfx/nvn/hle_nvn.c')
        cut = csrc.between(src, '// ---------- Programs ----------', '// ---------- Sync / events ----------', 'hle_nvn.c')
        cls._tmp = tempfile.TemporaryDirectory(prefix='nx2a_prog_keys_')
        tmp = Path(cls._tmp.name)
        c, cls.exe = tmp / 'check.c', tmp / 'check'
        c.write_text(HARNESS_HEAD + cut + HARNESS_TAIL, encoding='utf-8')
        nvn_dir = csrc.REPO / 'nxnative/dm/gfx/nvn'
        res = subprocess.run(
            [cc, '-std=gnu11', '-Wall', '-Wno-unused-function', '-Werror', f'-I{nvn_dir}', f'-I{nvn_dir / "include"}', str(c), '-lpthread', '-o', str(cls.exe)],
            capture_output=True, text=True,
        )
        if res.returncode != 0:
            cls._tmp.cleanup()
            raise AssertionError(res.stdout + res.stderr)

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, '_tmp'):
            cls._tmp.cleanup()

    def test_scenarios(self):
        for case in ('copy_after_set_shaders', 'keyed_later_from_host_copy', 'finalize_keeps_copy_keys', 'set_again_new_record'):
            with self.subTest(case=case):
                res = subprocess.run([str(self.exe), case], capture_output=True, text=True)
                self.assertEqual(res.returncode, 0, res.stdout + res.stderr)


if __name__ == '__main__':
    unittest.main()
