"""The uniform buffer component rewrite (nxnative/dm/android/gfx_vk_ubo_comp.c, DM_VK_UBO_COMPONENT) on hand-made SPIR-V modules, built with the host's C compiler.

spirv-val (on PATH, or SPIRV_VAL) also checks the modules before and after, when it is there.
"""
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_ubo_comp.c'
HARNESS = r'''
#define _CRT_SECURE_NO_WARNINGS   /* clang or MSVC with Microsoft's C library (a Windows runner): fopen is deprecated there, an error with -Werror */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
uint32_t *spv_ubo_component(const uint32_t *w, uint32_t nw, uint32_t *onw, uint32_t *nsites);
int main(int argc, char **argv) {   // harness <in.spv> <out.spv>: prints "rewritten <loads>" or "unchanged"
    (void)argc; FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *w = malloc((size_t)n); if (fread(w, 1, (size_t)n, f) != (size_t)n) return 2; fclose(f);
    uint32_t no = 0, ns = 0, *o = spv_ubo_component(w, (uint32_t)(n / 4), &no, &ns);
    if (o) { f = fopen(argv[2], "wb"); fwrite(o, 4, no, f); fclose(f); printf("rewritten %u\n", ns); } else puts("unchanged");
    return 0;
}
'''
CAP, MEMMODEL, ENTRY, EXECMODE, DECORATE, MEMBER_DECORATE = 17, 14, 15, 16, 71, 72
VOID, INT, VECTOR, ARRAY, STRUCT, POINTER, FUNCTYPE = 19, 21, 23, 28, 30, 32, 33
CONST = 43
FUNCTION, LABEL, RETURN, FUNCEND, VARIABLE, LOAD, STORE, CHAIN = 54, 248, 253, 56, 59, 61, 62, 65
EXTRACT_DYNAMIC, COMPOSITE_CONSTRUCT, IADD = 77, 80, 128
INPUT, UNIFORM, OUTPUT = 1, 2, 3


def ins(op, *args):
    return [(len(args) + 1) << 16 | op, *args]


def string(s):
    b = s.encode() + b'\0'
    b += b'\0' * (-len(b) % 4)
    return list(struct.unpack(f'<{len(b) // 4}I', b))


def insts(words):
    out, i = [], 5
    while i < len(words):
        out.append(words[i:i + (words[i] >> 16)])
        i += words[i] >> 16
    return out


class Ids:
    """i.<name>: a new id the first time a name is used."""
    def __init__(self):
        self.next = 1

    def __getattr__(self, name):
        if name.startswith('__'):
            raise AttributeError(name)
        setattr(self, name, self.next)
        self.next += 1
        return self.next - 1


def fragment(body):
    """A fragment shader with two flat uint inputs (row, comp), a uniform block of 8 uvec4 (ubo, set 0 binding 0) and a uvec4 output; body(i) is main's code."""
    i = Ids()
    words = [0x07230203, 0x10000, 0, 0, 0]
    words += ins(CAP, 1) + ins(MEMMODEL, 0, 1) + ins(ENTRY, 4, i.main, *string('main'), i.row, i.comp, i.out) + ins(EXECMODE, i.main, 7)
    for x in [ins(DECORATE, i.row, 30, 0), ins(DECORATE, i.row, 14), ins(DECORATE, i.comp, 30, 1), ins(DECORATE, i.comp, 14), ins(DECORATE, i.out, 30, 0),
              ins(DECORATE, i.arr, 6, 16), ins(MEMBER_DECORATE, i.blk, 0, 35, 0), ins(DECORATE, i.blk, 2),
              ins(DECORATE, i.ubo, 34, 0), ins(DECORATE, i.ubo, 33, 0),
              ins(VOID, i.void), ins(FUNCTYPE, i.fnty, i.void), ins(INT, i.uint, 32, 0), ins(VECTOR, i.v4, i.uint, 4), ins(CONST, i.uint, i.c0, 0),
              ins(CONST, i.uint, i.c1, 1), ins(CONST, i.uint, i.c8, 8), ins(ARRAY, i.arr, i.v4, i.c8), ins(STRUCT, i.blk, i.arr), ins(POINTER, i.pblk, UNIFORM, i.blk),
              ins(POINTER, i.pu, UNIFORM, i.uint), ins(POINTER, i.pin, INPUT, i.uint), ins(POINTER, i.pout, OUTPUT, i.v4),
              ins(VARIABLE, i.pblk, i.ubo, UNIFORM), ins(VARIABLE, i.pin, i.row, INPUT), ins(VARIABLE, i.pin, i.comp, INPUT), ins(VARIABLE, i.pout, i.out, OUTPUT),
              ins(FUNCTION, i.void, i.main, 0, i.fnty), ins(LABEL, i.entry), *body(i), ins(RETURN), ins(FUNCEND)]:
        words += x
    words[3] = i.next
    return words


def per_pixel(i):   # ubo.arr[row].<comp + 1>, both differing per pixel
    return [ins(LOAD, i.uint, i.r, i.row), ins(LOAD, i.uint, i.c, i.comp), ins(IADD, i.uint, i.cc, i.c, i.c1), ins(CHAIN, i.pu, i.p, i.ubo, i.c0, i.r, i.cc),
            ins(LOAD, i.uint, i.v, i.p), ins(COMPOSITE_CONSTRUCT, i.v4, i.o, i.v, i.v, i.v, i.v), ins(STORE, i.out, i.o)]


class UboComponent(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
        if not cc:
            raise unittest.SkipTest('a C compiler is needed for the uniform buffer component check')
        cls.tmp = tempfile.TemporaryDirectory(prefix='vk-ubo-comp-')
        cls.exe = Path(cls.tmp.name) / 'ubo_comp'
        (Path(cls.tmp.name) / 'harness.c').write_text(HARNESS)
        subprocess.run([cc, '-std=gnu11', '-Wall', '-Werror', '-o', str(cls.exe), str(Path(cls.tmp.name) / 'harness.c'), str(SOURCE)], check=True)
        cls.val = os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def rewrite(self, words):
        self.validate(words)
        src, dst = Path(self.tmp.name) / 'in.spv', Path(self.tmp.name) / 'out.spv'
        src.write_bytes(struct.pack(f'<{len(words)}I', *words))
        dst.unlink(missing_ok=True)
        r = subprocess.run([str(self.exe), str(src), str(dst)], capture_output=True, text=True, check=True).stdout.strip()
        out = list(struct.unpack(f'<{dst.stat().st_size // 4}I', dst.read_bytes())) if r.startswith('rewritten') else None
        if out:
            self.validate(out)
        return r, out

    def validate(self, words):
        if not self.val:
            return
        r = subprocess.run([self.val, '--target-env', 'vulkan1.0', '-'], input=struct.pack(f'<{len(words)}I', *words), capture_output=True)
        self.assertEqual(r.returncode, 0, r.stdout.decode() + r.stderr.decode())

    def test_run_time_component_from_vector(self):
        words = fragment(per_pixel)
        r, out = self.rewrite(words)
        self.assertEqual(r, 'rewritten 1')
        x = insts(out)
        ptr = [k for k in x if k[0] & 0xffff == POINTER and k[2] == UNIFORM and k[3] == x[[k[0] & 0xffff for k in x].index(VECTOR)][1]]
        self.assertEqual(len(ptr), 1)   # a Uniform pointer to the uvec4, added
        chain = [k for k in x if k[0] & 0xffff == CHAIN]
        self.assertEqual(len(chain), 1)
        self.assertEqual(len(chain[0]), 6)   # base, member 0, row: the component index is gone
        self.assertEqual(chain[0][1], ptr[0][1])
        ext = [k for k in x if k[0] & 0xffff == EXTRACT_DYNAMIC]
        self.assertEqual(len(ext), 1)
        self.assertEqual(out[3], words[3] + 3)   # the pointer type, the chain and the vector load

    def test_constant_component_unchanged(self):   # a dynamic row with a constant component: the driver took it right
        r, _ = self.rewrite(fragment(lambda i: [ins(LOAD, i.uint, i.r, i.row), ins(CHAIN, i.pu, i.p, i.ubo, i.c0, i.r, i.c1), ins(LOAD, i.uint, i.v, i.p),
                                                ins(COMPOSITE_CONSTRUCT, i.v4, i.o, i.v, i.v, i.v, i.v), ins(STORE, i.out, i.o)]))
        self.assertEqual(r, 'unchanged')

    def test_pointer_used_twice_unchanged(self):
        def body(i):
            return [ins(LOAD, i.uint, i.c, i.comp), ins(CHAIN, i.pu, i.p, i.ubo, i.c0, i.c1, i.c), ins(LOAD, i.uint, i.v, i.p), ins(LOAD, i.uint, i.v2, i.p),
                    ins(COMPOSITE_CONSTRUCT, i.v4, i.o, i.v, i.v2, i.v, i.v), ins(STORE, i.out, i.o)]
        r, _ = self.rewrite(fragment(body))
        self.assertEqual(r, 'unchanged')

if __name__ == '__main__':
    unittest.main()
