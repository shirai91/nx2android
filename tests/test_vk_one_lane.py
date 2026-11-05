"""The one-invocation subgroup rewrite (nxnative/dm/android/gfx_vk_subgroup.c) on hand-made SPIR-V modules, built with the host's C compiler.

spirv-val (on PATH, or SPIRV_VAL) also checks the modules before and after, when it is there.
"""
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_subgroup.c'
HARNESS = r'''
#define _CRT_SECURE_NO_WARNINGS   /* clang or MSVC with Microsoft's C library (a Windows runner): fopen is deprecated there, an error with -Werror */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
uint32_t *spv_one_lane(const uint32_t *w, uint32_t nw, uint32_t ops, uint32_t *onw, char *why, size_t nwhy);
int main(int argc, char **argv) {   // harness <in.spv> <ops> <out.spv>: prints "rewritten", "unchanged" or the reason
    (void)argc; FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *w = malloc((size_t)n); if (fread(w, 1, (size_t)n, f) != (size_t)n) return 2; fclose(f);
    char why[64]; uint32_t no = 0, *o = spv_one_lane(w, (uint32_t)(n / 4), (uint32_t)strtoul(argv[2], NULL, 0), &no, why, sizeof why);
    if (o) { f = fopen(argv[3], "wb"); fwrite(o, 4, no, f); fclose(f); puts("rewritten"); } else puts(why[0] ? why : "unchanged");
    return 0;
}
'''
# opcodes and operands used below
CAP, EXT, MEMMODEL, ENTRY, EXECMODE, DECORATE = 17, 10, 14, 15, 16, 71
VOID, BOOL, INT, VECTOR, POINTER, FUNCTYPE = 19, 20, 21, 23, 32, 33
CONST_TRUE, CONST, COMPOSITE, NULL = 41, 43, 44, 46
FUNCTION, LABEL, RETURN, FUNCEND, VARIABLE, LOAD, STORE, CHAIN = 54, 248, 253, 56, 59, 61, 62, 65
COMPOSITE_CONSTRUCT, COPY, SELECT, IEQUAL = 80, 83, 169, 170
INPUT, OUTPUT, PRIVATE = 1, 3, 6
SG_OPS = set(range(333, 367)) | {4421, 4422, 4428, 4429, 4430, 4431, 4432}
WITH_TYPE = {CONST_TRUE, CONST, COMPOSITE, NULL, VARIABLE, LOAD, CHAIN, COMPOSITE_CONSTRUCT, COPY, SELECT, IEQUAL} | SG_OPS   # result type, then result id


def ins(op, *args):
    return [(len(args) + 1) << 16 | op, *args]


def string(s):
    b = s.encode() + b'\0'
    b += b'\0' * (-len(b) % 4)
    return list(struct.unpack(f'<{len(b) // 4}I', b))


def parse(words):
    out, i = [], 5
    while i < len(words):
        n = words[i] >> 16
        out.append(list(words[i:i + n]))
        i += n
    return out


def results(insts):   # (result id, instruction) of the instructions this test makes
    for x in insts:
        op = x[0] & 0xffff
        if VOID <= op <= FUNCTYPE or op == LABEL:
            yield x[1], x
        elif op in WITH_TYPE or op == FUNCTION:
            yield x[2], x


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


def module(i, decls, body, interface=(), caps=(1,), exts=(), decorations=(), version=0x10300):
    """A fragment shader: the capabilities, extensions, entry point main with that interface, the decorations, the declarations, then main's body."""
    words = [0x07230203, version, 0, 0, 0]
    for c in caps:
        words += ins(CAP, c)
    for e in exts:
        words += ins(EXT, *string(e))
    words += ins(MEMMODEL, 0, 1) + ins(ENTRY, 4, i.main, *string('main'), *interface, i.out) + ins(EXECMODE, i.main, 7)
    for x in [*decorations, ins(DECORATE, i.out, 30, 0), ins(VOID, i.void), ins(FUNCTYPE, i.fnty, i.void), ins(BOOL, i.bool), ins(INT, i.uint, 32, 0),
              ins(VECTOR, i.v4, i.uint, 4), ins(POINTER, i.pout4, OUTPUT, i.v4), ins(CONST_TRUE, i.bool, i.true), *decls,
              ins(VARIABLE, i.pout4, i.out, OUTPUT), ins(FUNCTION, i.void, i.main, 0, i.fnty), ins(LABEL, i.entry), *body, ins(RETURN), ins(FUNCEND)]:
        words += x
    words[3] = i.next
    return words


def fragment(version=0x10300, extra=lambda i: [], builtins=((41, 'lane'), (36, 'size'), (4416, 'eq'), (4420, 'lt'))):
    """Reads the subgroup built-ins (the less mask through an access chain) and uses every covered OpGroupNonUniform operation."""
    i = Ids()
    bvars = [(b, getattr(i, name)) for b, name in builtins]
    decls = [ins(POINTER, i.pin, INPUT, i.uint), ins(POINTER, i.pin4, INPUT, i.v4), ins(CONST, i.uint, i.c3, 3), ins(CONST, i.uint, i.c2, 2),
             *[ins(VARIABLE, i.pin4 if b >= 4416 else i.pin, v, INPUT) for b, v in bvars]]
    body = [ins(LOAD, i.uint, i.l, i.lane), ins(LOAD, i.uint, i.s, i.size), ins(LOAD, i.v4, i.e, i.eq), ins(CHAIN, i.pin, i.chain, i.lt, i.c2),
            ins(LOAD, i.uint, i.lw, i.chain), ins(IEQUAL, i.bool, i.p, i.l, i.s),
            ins(339, i.v4, i.ballot, i.c3, i.p), ins(335, i.bool, i.any, i.c3, i.p), ins(334, i.bool, i.all, i.c3, i.any),
            ins(336, i.bool, i.alleq, i.c3, i.l), ins(333, i.bool, i.elect, i.c3), ins(345, i.uint, i.shfl, i.c3, i.l, i.s),
            ins(346, i.uint, i.xor, i.c3, i.shfl, i.c2), ins(347, i.uint, i.up, i.c3, i.xor, i.c2), ins(337, i.uint, i.bcast, i.c3, i.up, i.c2),
            ins(338, i.uint, i.first, i.c3, i.bcast), ins(366, i.uint, i.quad, i.c3, i.first, i.c2), *extra(i),
            ins(COMPOSITE_CONSTRUCT, i.v4, i.v, i.quad, i.lw, i.l, i.s), ins(STORE, i.out, i.ballot), ins(STORE, i.out, i.v), ins(STORE, i.out, i.e)]
    decorations = [d for b, v in bvars for d in (ins(DECORATE, v, 11, b), ins(DECORATE, v, 14))]   # BuiltIn, Flat
    caps = (1, 61, 62, 64, 65, 66, 68)   # Shader, GroupNonUniform, Vote, Ballot, Shuffle, ShuffleRelative, Quad
    return module(i, decls, body, [v for _, v in bvars], caps, (), decorations, version), i


class OneLane(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
        if not cc:
            raise unittest.SkipTest('a C compiler is needed for the one-invocation rewrite check')
        cls.tmp = tempfile.TemporaryDirectory(prefix='vk-one-lane-')
        cls.exe = Path(cls.tmp.name) / 'one_lane'
        (Path(cls.tmp.name) / 'harness.c').write_text(HARNESS)
        subprocess.run([cc, '-std=gnu11', '-Wall', '-Werror', '-o', str(cls.exe), str(Path(cls.tmp.name) / 'harness.c'), str(SOURCE)], check=True)
        cls.val = os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_rewrite(self, words, ops=0):
        src, dst = Path(self.tmp.name) / 'in.spv', Path(self.tmp.name) / 'out.spv'
        src.write_bytes(struct.pack(f'<{len(words)}I', *words))
        dst.unlink(missing_ok=True)
        r = subprocess.run([str(self.exe), str(src), hex(ops), str(dst)], capture_output=True, text=True, check=True).stdout.strip()
        return r, (list(struct.unpack(f'<{dst.stat().st_size // 4}I', dst.read_bytes())) if r == 'rewritten' else None)

    def validate(self, words, env='vulkan1.1'):
        if not self.val:
            return
        r = subprocess.run([self.val, '--target-env', env, '-'], input=struct.pack(f'<{len(words)}I', *words), capture_output=True)
        self.assertEqual(r.returncode, 0, (r.stdout + r.stderr).decode(errors='replace'))

    def value(self, d, cid):   # a constant's value: an int, a tuple for a composite, True
        x = d[cid]
        op = x[0] & 0xffff
        if op == CONST:
            return x[3]
        if op == CONST_TRUE:
            return True
        if op == NULL:
            return (0,) * d[x[1]][3] if d[x[1]][0] & 0xffff == VECTOR else 0
        if op == COMPOSITE:
            return tuple(self.value(d, c) for c in x[3:])
        self.fail(f'{cid} is not a constant: {x}')

    def test_fragment_rewritten(self):
        words, i = fragment()
        self.validate(words)
        r, out = self.run_rewrite(words)
        self.assertEqual(r, 'rewritten')
        self.validate(out)
        insts = parse(out)
        ids = [r for r, _ in results(insts)]
        d = dict(results(insts))
        self.assertEqual(len(out), 5 + sum(len(x) for x in insts))
        self.assertEqual(len(ids), len(set(ids)), 'a result id defined twice')
        self.assertGreater(out[3], max(ids), 'the bound covers the new ids')
        self.assertFalse([x for x in insts if x[0] & 0xffff in SG_OPS], 'subgroup operations left')
        self.assertEqual([x[1] for x in insts if x[0] & 0xffff == CAP], [1], 'only Shader is left')
        # the built-ins: Private variables holding their one-invocation values, undecorated, out of the 1.3 interface
        want = {i.lane: 0, i.size: 1, i.eq: (1, 0, 0, 0), i.lt: (0, 0, 0, 0)}
        for v, value in want.items():
            self.assertEqual(d[v][3], PRIVATE)
            self.assertEqual(d[d[v][1]][2], PRIVATE)
            self.assertEqual(self.value(d, d[v][4]), value)
        self.assertFalse([x for x in insts if x[0] & 0xffff == DECORATE and x[1] in want])
        entry = next(x for x in insts if x[0] & 0xffff == ENTRY)
        self.assertEqual((entry[0] >> 16, entry[3:]), (len(entry), string('main') + [i.out]))
        # the chain into the less mask points to Private memory now; loads stay loads
        self.assertEqual(d[d[i.chain][1]][2:], [PRIVATE, i.uint])
        self.assertEqual(d[i.lw][0] & 0xffff, LOAD)
        # the operations: the invocation's own value, the predicate, true, and its ballot (the predicate in bit 0)
        for res, src in {i.any: i.p, i.all: i.any, i.shfl: i.l, i.xor: i.shfl, i.up: i.xor, i.bcast: i.up, i.first: i.bcast, i.quad: i.first}.items():
            self.assertEqual((d[res][0] & 0xffff, d[res][3]), (COPY, src))
        for res in (i.alleq, i.elect):
            self.assertEqual(d[res][0] & 0xffff, COPY)
            self.assertIs(self.value(d, d[res][3]), True)
        ballot = d[i.ballot]
        self.assertEqual((ballot[0] & 0xffff, ballot[1]), (COMPOSITE_CONSTRUCT, i.v4))
        self.assertEqual([self.value(d, c) for c in ballot[4:]], [0, 0, 0])
        sel = d[ballot[3]]
        self.assertEqual((sel[0] & 0xffff, sel[1], sel[3], self.value(d, sel[4]), self.value(d, sel[5])), (SELECT, i.uint, i.p, 1, 0))

    def test_spirv_1_4_keeps_the_interface(self):
        words, i = fragment(version=0x10400)
        self.validate(words, 'vulkan1.1spv1.4')
        r, out = self.run_rewrite(words)
        self.assertEqual(r, 'rewritten')
        self.validate(out, 'vulkan1.1spv1.4')
        entry = next(x for x in parse(out) if x[0] & 0xffff == ENTRY)
        self.assertEqual(entry[-5:], [i.lane, i.size, i.eq, i.lt, i.out])   # from 1.4 an interface lists every global its entry point uses

    def test_stage_with_the_classes_left_alone(self):
        words, _ = fragment()
        self.assertEqual(self.run_rewrite(words, ops=0xff), ('unchanged', None))   # basic, vote, arithmetic, ballot, shuffle, relative, clustered, quad
        r, out = self.run_rewrite(words, ops=0xff & ~0x20)   # without shuffle relative (ShuffleUp): the whole module
        self.assertEqual(r, 'rewritten')
        self.assertFalse([x for x in parse(out) if x[0] & 0xffff in SG_OPS])

    def test_module_without_subgroups_unchanged(self):
        i = Ids()   # (its capabilities declare subgroups, as the translator's always do)
        words = module(i, [ins(NULL, i.v4, i.zero)], [ins(STORE, i.out, i.zero)], caps=(1, 62, 64, 65))
        self.validate(words)
        self.assertEqual(self.run_rewrite(words), ('unchanged', None))

    def test_uncovered_operation_refused(self):
        words, _ = fragment(extra=lambda i: [ins(342, i.uint, i.count, i.c3, 0, i.ballot)])   # OpGroupNonUniformBallotBitCount Reduce
        self.assertEqual(self.run_rewrite(words), ('opcode 342', None))

    def test_uncovered_builtin_refused(self):
        words, _ = fragment(builtins=((41, 'lane'), (36, 'size'), (4416, 'eq'), (4420, 'lt'), (40, 'sgid')))   # SubgroupId
        self.assertEqual(self.run_rewrite(words), ('built-in 40', None))

    def test_khr_ballot_and_vote(self):
        i = Ids()
        words = module(i, [], [ins(4428, i.bool, i.all, i.true), ins(4421, i.v4, i.ballot, i.all), ins(STORE, i.out, i.ballot)],   # SubgroupAllKHR, SubgroupBallotKHR
                       caps=(1, 4423, 4431), exts=('SPV_KHR_shader_ballot', 'SPV_KHR_subgroup_vote'))
        self.validate(words)
        r, out = self.run_rewrite(words)
        self.assertEqual(r, 'rewritten')
        self.validate(out)
        insts = parse(out)
        d = dict(results(insts))
        self.assertEqual([x[1] for x in insts if x[0] & 0xffff == CAP], [1])
        self.assertFalse([x for x in insts if x[0] & 0xffff == EXT])
        self.assertEqual((d[i.all][0] & 0xffff, d[i.all][3]), (COPY, i.true))
        self.assertEqual(d[i.ballot][0] & 0xffff, COMPOSITE_CONSTRUCT)
        self.assertEqual(d[d[i.ballot][3]][3], i.all)


if __name__ == '__main__':
    unittest.main()
