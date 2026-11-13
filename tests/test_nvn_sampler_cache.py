"""Opaque sampler hashes must include every parameter used by SamplerCompare."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'nxnative/dm/gfx/nvn'


def harness():
    source = (NVN / 'hle_nvn.c').read_text(encoding='utf-8')

    def function(marker):
        start = source.index(marker)
        body = source.index('{', start)
        depth = 0
        for token in re.finditer(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|[{}]', source[body:]):
            if token[0] == '{':
                depth += 1
            elif token[0] == '}':
                depth -= 1
                if not depth:
                    return source[start:body + token.end()]
        raise AssertionError(marker)

    pre = r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "nvn_obj.h"
#define NVN(name, ret, ...) ret h_##name(__VA_ARGS__)
#define SP(s) (&((const samp_t *)(s))->p)
'''
    functions = [function('NVN(' + name + ',') for name in
                 ['nvnSamplerInitialize', 'nvnSamplerFinalize', 'nvnSamplerCompare']]
    post = r'''
// The engine's CRC16 skips the first eight opaque bytes. Its larger backing
// record is zero initialized; the host must stay within the public NVN object.
static uint16_t opaque_hash(const unsigned char *object, size_t bytes) {
    uint16_t crc = 0;
    for (size_t i = 8; i < bytes; i++) {
        crc ^= object[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ ((crc & 1) ? 0xa001 : 0);
    }
    return crc;
}
typedef union { uint64_t align; unsigned char data[96]; } cached_sampler;
static cached_sampler make(const samp_par *parameters) {
    cached_sampler object = {0};
    samp_builder_t builder = {0}; builder.p = *parameters;
    assert(h_nvnSamplerInitialize((NVNsampler *)object.data, (const NVNsamplerBuilder *)&builder));
    return object;
}
static int compare(const void *a, const void *b) {
    return (int)opaque_hash(((const cached_sampler *)a)->data, 96) -
           (int)opaque_hash(((const cached_sampler *)b)->data, 96);
}
int main(void) {
    assert(sizeof(samp_t) <= sizeof(NVNsampler));
    assert(offsetof(samp_t, p) == 8);
    samp_par p = {0}; p.minf = 5; p.magf = 1; p.ws = p.wt = p.wr = 7;
    p.cmp_func = 1; p.lod_max = 13; p.aniso = 1;
    cached_sampler original = make(&p), copy = make(&p);
    assert(!memcmp(original.data, copy.data, sizeof original.data));
    assert(h_nvnSamplerCompare((NVNsampler *)original.data, (NVNsampler *)copy.data));
    // Isolate all four fields previously hidden in the skipped header.
    for (int field = 0; field < 4; field++) {
        samp_par q = p; ((unsigned char *)&q)[field] ^= 1;
        cached_sampler other = make(&q);
        assert(!h_nvnSamplerCompare((NVNsampler *)original.data, (NVNsampler *)other.data));
        assert(opaque_hash(original.data, 96) != opaque_hash(other.data, 96));
        assert(opaque_hash(original.data, sizeof(NVNsampler)) != opaque_hash(other.data, sizeof(NVNsampler)));
    }
    // Guard bytes after a small public object: larger engine records must not
    // tempt initialization/finalization to write beyond the header's capacity.
    union { uint64_t align; unsigned char bytes[sizeof(NVNsampler) + 16]; } guarded;
    memset(&guarded, 0xa5, sizeof guarded);
    samp_builder_t builder = {0}; builder.p = p;
    h_nvnSamplerInitialize((NVNsampler *)guarded.bytes, (const NVNsamplerBuilder *)&builder);
    h_nvnSamplerFinalize((NVNsampler *)guarded.bytes);
    for (size_t i = sizeof(NVNsampler); i < sizeof guarded.bytes; i++) assert(guarded.bytes[i] == 0xa5);
    // Model the observed hash-only binary lookup followed by semantic equality.
    // Colliding filter modes previously allocated duplicates until the pool ran out.
    cached_sampler cache[64]; size_t used = 0;
    for (int frame = 0; frame < 10000; frame++) {
        samp_par q = p; q.minf = frame % 2 ? 3 : 5;
        cached_sampler request = make(&q);
        cached_sampler *found = bsearch(&request, cache, used, sizeof cache[0], compare);
        if (found && h_nvnSamplerCompare((NVNsampler *)found->data, (NVNsampler *)request.data)) continue;
        assert(used < 64); cache[used++] = request;
        qsort(cache, used, sizeof cache[0], compare);
    }
    assert(used == 2);
    puts("sampler hash/cache reuse and object bounds: PASS (10000 requests, 2 entries)");
    return 0;
}
'''
    return pre + '\n'.join(functions) + post


class SamplerCacheTests(unittest.TestCase):
    def test_opaque_hash_and_cache_reuse(self):
        cc = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
        if not cc:
            self.skipTest('Native C compiler unavailable; Android harness can run separately')
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'check.c'
            exe = Path(directory) / 'check.exe'
            source.write_text(harness(), encoding='utf-8')
            subprocess.run([cc, '-std=c11', '-O2', '-I' + str(NVN), '-I' + str(NVN / 'include'),
                            str(source), '-o', str(exe)], check=True, capture_output=True)
            result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
            self.assertIn('PASS (10000 requests, 2 entries)', result.stdout)
