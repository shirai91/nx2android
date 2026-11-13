"""Function patch recognition through real AArch64 code and synthetic unwind tables.
No game files. Relocation is accepted; changed logic, callees, ambiguity and wrong
profile/module selection are refused through the rewriter's actual registry code.
A patch of several functions applies whole or not at all. Registry/cache integration is tested separately.
"""
import ast
import contextlib
import copy
import glob
import importlib.util
import io
import os
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/tools'
sys.path.insert(0, str(TOOLS))
from nso_asm import ADRP, ADDI, B, NOP
from nso_match import Image, MatchError, resolve, validate

MOV = 0xaa1703e0  # mov x0, x23
ADD = 0x914012e0  # add x0, x23, #0x4000
RET = 0xd65f03c0


def image(functions):
    """A MOD0 and .eh_frame_hdr describing the supplied functions."""
    out = bytearray(0x4000)
    struct.pack_into('<I', out, 4, 0x20)
    out[0x20:0x24] = b'MOD0'
    struct.pack_into('<ii', out, 0x30, 0x3000 - 0x20, 0x300c + len(functions) * 8 - 0x20)
    out[0x3000:0x3004] = bytes((1, 0x1b, 3, 0x3b))
    struct.pack_into('<iI', out, 0x3004, 0x3200 - 0x3004, len(functions))
    # A zR CIE with pcrel/sdata4 PCs and four-byte lengths.
    struct.pack_into('<II', out, 0x3200, 16, 0)
    out[0x3208:0x3211] = b'\x01zR\0\x04\x78\x1e\x01\x1b'
    for i, (start, words) in enumerate(sorted(functions.items())):
        struct.pack_into('<' + 'I' * len(words), out, start, *words)
        fde = 0x3240 + i * 24
        struct.pack_into('<ii', out, 0x300c + i * 8, start - 0x3000, fde - 0x3000)
        struct.pack_into('<IIiI', out, fde, 16, fde + 4 - 0x3200, start - (fde + 8), len(words) * 4)
    return out


def caller(start, target, data=0x2200):
    return [ADRP(8, start, data), ADDI(8, 8, data & 0xfff), MOV,
            B(start + 12, target) | 0x80000000, 0x54000040, NOP, RET]  # bl; b.eq +8


def imported(name, plt=0x1800, slot=0x2500):
    out = image({0x100: caller(0x100, plt)})
    struct.pack_into('<i', out, 0x24, 0x2100 - 0x20)
    for i, (tag, value) in enumerate(((20, 7), (23, 0x2200), (2, 24), (6, 0x2300), (5, 0x2400), (0, 0))):
        struct.pack_into('<qQ', out, 0x2100 + i * 16, tag, value)
    struct.pack_into('<QQq', out, 0x2200, slot, (1 << 32) | 1026, 0)
    struct.pack_into('<I', out, 0x2318, 1)
    data = b'\0' + name.encode() + b'\0'; out[0x2400:0x2400 + len(data)] = data
    lo = slot & 0xfff
    struct.pack_into('<4I', out, plt, ADRP(16, plt, slot), 0xf9400211 | (lo // 8) << 10, ADDI(16, 16, lo), 0xd61f0220)
    return out


class Matching(unittest.TestCase):
    def setUp(self):
        self.original = image({0x100: caller(0x100, 0x500), 0x500: [0xd2800020, RET]})
        self.rule = Image(self.original, 0x2000).recipe(0x108, 'e00317aa', 'e0124091', 'heap')

    def test_relocated_function_callee_and_data_addresses(self):
        new = image({0x800: caller(0x800, 0x1200, 0x2980), 0x1200: [0xd2800020, RET]})
        before = bytes(new)
        cuts = resolve(new, 0x2000, [self.rule], lambda _: None)
        self.assertEqual(cuts[0][:3], (0x808, 'e00317aa', 'e0124091'))
        self.assertEqual(bytes(new), before)  # selection never writes partially verified patches

    def test_changed_logic_and_callee_are_not_matches(self):
        for off, word in ((0x108, 0xaa1603e0), (0x110, 0x54000020), (0x500, 0xd2800040)):
            with self.subTest(off=hex(off)):
                changed = bytearray(self.original); struct.pack_into('<I', changed, off, word)
                with self.assertRaisesRegex(MatchError, '0 matching'):
                    Image(changed, 0x2000).match(self.rule)

    def test_call_redirected_to_different_function(self):
        changed = image({0x100: caller(0x100, 0x600), 0x500: [0xd2800020, RET], 0x600: [0xd2800040, RET]})
        with self.assertRaisesRegex(MatchError, '0 matching'): Image(changed, 0x2000).match(self.rule)

    def test_import_identity_survives_relocation_but_not_redirection(self):
        old = Image(imported('allocate'), 0x2000)
        rule = old.recipe(0x108, 'e00317aa', 'e0124091', 'heap')
        moved = Image(imported('allocate', plt=0x1900, slot=0x2800), 0x2000)
        self.assertEqual(moved.match(rule)[0][0], 0x108)
        with self.assertRaisesRegex(MatchError, '0 matching'):
            Image(imported('free'), 0x2000).match(rule)

    def test_system_call_wrapper_is_identified_by_its_number(self):
        # the SDK's svc wrappers have no unwind entry: `svc #n; ret` stands for call n, and another number is another callee
        def module(svc, at=0x500):
            out = image({0x100: caller(0x100, at)})
            struct.pack_into('<2I', out, at, 0xd4000001 | svc << 5, RET)
            return out
        rule = Image(module(0x2c), 0x2000).recipe(0x108, 'e00317aa', 'e0124091', 'heap')
        self.assertEqual(Image(module(0x2c, 0x700), 0x2000).match(rule)[0][0], 0x108)
        with self.assertRaisesRegex(MatchError, '0 matching'): Image(module(0x2d), 0x2000).match(rule)
        longer = module(0x2c); struct.pack_into('<I', longer, 0x504, NOP)   # not a bare wrapper: no identity, no match
        with self.assertRaisesRegex(MatchError, 'no verified boundary'): Image(longer, 0x2000).fingerprint(0x100)

    def test_address_register_clobbers_preserve_member_offsets(self):
        for reg, move in ((8, 0x2a0003e8), (29, 0xaa0003fd)):
            words = [ADRP(reg, 0x100, 0x2000), move, 0xf9400000 | (reg << 5) | (8 << 10), MOV, RET]
            old = image({0x100: words})
            rule = Image(old, 0x2000).recipe(0x10c, 'e00317aa', 'e0124091', 'heap')
            words[2] += 1 << 10  # changed field offset after the address was overwritten
            with self.assertRaisesRegex(MatchError, '0 matching'):
                Image(image({0x100: words}), 0x2000).match(rule)

    def test_internal_adr_target_is_not_normalized(self):
        old = image({0x100: [MOV, 0x10000040, NOP, RET]})  # adr x0, function+12
        rule = Image(old, 0x2000).recipe(0x100, 'e00317aa', 'e0124091', 'heap')
        new = image({0x100: [MOV, 0x10000020, NOP, RET]})  # adr x0, function+8
        with self.assertRaisesRegex(MatchError, '0 matching'): Image(new, 0x2000).match(rule)

    def test_ambiguous_function_and_missing_callee_boundary(self):
        duplicate = image({0x100: caller(0x100, 0x500), 0x800: caller(0x800, 0x500), 0x500: [0xd2800020, RET]})
        with self.assertRaisesRegex(MatchError, '2 matching'): Image(duplicate, 0x2000).match(self.rule)
        missing = image({0x100: caller(0x100, 0x500)})
        with self.assertRaisesRegex(MatchError, '0 matching'): Image(missing, 0x2000).match(self.rule)

    def test_ambiguous_alternative_refuses_the_whole_named_patch(self):
        a, b, common = [MOV, RET], [MOV, NOP, RET], [NOP, RET]
        for multi in (False, True):
            with self.subTest(multi=multi):
                def recipe(words):
                    module = Image(image({0x100: words, 0x600: common}), 0x2000)
                    cuts = [(0x100, 'e00317aa', 'e0124091', 'size')]
                    if multi: cuts.append((0x600, '1f2003d5', '1f2003d5', 'bound'))
                    return module.patch_recipe(cuts, 'region')
                rules = [recipe(a), recipe(b)]
                ambiguous = image({0x100: a, 0x200: a, 0x300: b, 0x600: common})
                before, messages, outcomes = bytes(ambiguous), [], []
                self.assertEqual(resolve(ambiguous, 0x2000, rules, messages.append, outcomes), [])
                self.assertEqual(bytes(ambiguous), before)
                self.assertIn('2 matching functions', messages[0])
                self.assertEqual(outcomes[0][:2], ('region', None))

    def test_original_bytes_still_required(self):
        rule = copy.deepcopy(self.rule); rule['cuts'][0] = (8, '00000000', 'e0124091', 'heap')
        with self.assertRaisesRegex(MatchError, 'original bytes differ'): Image(self.original, 0x2000).match(rule)

    def test_relocations_in_replacement_are_rejected(self):
        for w in (B(0, 0x1000), ADRP(0, 0, 0x1000), 0x58000020):
            rule = copy.deepcopy(self.rule); rule['cuts'][0] = (8, 'e00317aa', struct.pack('<I', w).hex(), 'heap')
            with self.assertRaisesRegex(ValueError, 'PC-relative'): validate(rule)

    def test_replacement_branches_must_stay_inside_the_function(self):
        def cut(w): return (8, 'e00317aa', struct.pack('<I', w).hex(), 'heap')
        for target in (-8, 16):  # the function's first instruction and its last, from the cut at +8
            rule = copy.deepcopy(self.rule); rule['cuts'][0] = cut(B(0, target)); validate(rule)
        for target in (-12, 20):  # before the function and just past it
            rule = copy.deepcopy(self.rule); rule['cuts'][0] = cut(B(0, target))
            with self.assertRaisesRegex(ValueError, 'PC-relative'): validate(rule)
        # The second word of a replacement counts from its own place
        rule = copy.deepcopy(self.rule); rule['cuts'][0] = (8, 'e00317aa' + '00000014', struct.pack('<II', NOP, B(0, 16)).hex(), 'heap')
        with self.assertRaisesRegex(ValueError, 'PC-relative'): validate(rule)

    def test_trap_word_is_read_and_fingerprinted(self):
        words = caller(0x100, 0x500)
        def fingerprint(callee): return Image(image({0x100: words, 0x500: callee}), 0x2000).fingerprint(0x100)
        trapped = fingerprint([0xd2800020, RET, 0xe7ffdefe])
        self.assertNotEqual(trapped, fingerprint([0xd2800020, RET, 0xd4200000]))   # the word itself counts, not the brk it is read as
        with self.assertRaisesRegex(MatchError, 'undecodable'): fingerprint([0xd2800020, RET, 0xffffffff])  # any other undecodable word still refuses

    def test_no_boundaries_and_ambiguous_match_are_reported(self):
        for img in (bytearray(0x40), image({0x100: [RET]})):
            messages = []
            self.assertEqual(resolve(img, len(img), [self.rule], messages.append), [])
            self.assertTrue(messages[0].startswith('[RED] '))

    def test_overlapping_rules_are_rejected(self):
        other = dict(self.rule, name='another patch')
        with self.assertRaisesRegex(MatchError, 'overlap'):
            resolve(self.original, 0x2000, [self.rule, other], lambda _: None)

    def test_same_name_rules_are_alternative_implementations(self):
        # A later build changed the function (another size, the cut elsewhere in it): one rule per reviewed implementation, under one name
        def changed(start):  # caller() with one more instruction before the cut
            return [ADRP(8, start, 0x2200), ADDI(8, 8, 0x200), NOP, MOV, B(start + 16, 0x500) | 0x80000000, 0x54000040, NOP, RET]
        later = image({0x100: changed(0x100), 0x500: [0xd2800020, RET]})
        alt = Image(later, 0x2000).recipe(0x10c, 'e00317aa', 'e0124091', 'heap')
        self.assertNotEqual(alt['size'], self.rule['size'])
        for img, off in ((self.original, 0x108), (later, 0x10c)):
            with self.subTest(off=hex(off)):
                messages = []
                cuts = resolve(img, 0x2000, [self.rule, alt], messages.append)
                self.assertEqual([c[0] for c in cuts], [off])
                self.assertFalse([m for m in messages if m.startswith('[RED]')], messages)
        messages = []
        self.assertEqual(resolve(image({0x100: [RET]}), 0x2000, [self.rule, alt], messages.append), [])
        self.assertEqual(len(messages), 1); self.assertTrue(messages[0].startswith('[RED] '))
        both = image({0x100: caller(0x100, 0x500), 0x800: changed(0x800), 0x500: [0xd2800020, RET]})
        messages = []
        self.assertEqual(resolve(both, 0x2000, [self.rule, alt], messages.append), [])
        self.assertIn('2 implementations match', messages[0])







class Patches(unittest.TestCase):
    """A patch of several functions: one constant whose uses lie in different functions applies whole or not at all"""
    def setUp(self):
        self.original = image({0x100: caller(0x100, 0x500), 0x500: [0xd2800020, RET], 0x800: [MOV, NOP, RET]})
        self.rule = Image(self.original, 0x2000).patch_recipe([(0x108, 'e00317aa', 'e0124091', 'size'), (0x800, 'e00317aa', 'e0124091', 'bound')], 'region')

    def test_recipe_spans_functions(self):
        self.assertEqual(set(self.rule), {'name', 'functions'})
        self.assertEqual([(f['size'], [c[0] for c in f['cuts']]) for f in self.rule['functions']], [(28, [8]), (12, [0])])
        one = Image(self.original, 0x2000).patch_recipe([(0x108, 'e00317aa', 'e0124091', 'size')], 'region')
        self.assertEqual(set(one), {'name', 'size', 'fingerprints', 'cuts'})  # one function: the single form
        self.assertEqual(one['cuts'], [(8, 'e00317aa', 'e0124091', 'size')])

    def test_relocated_functions_match_together(self):
        moved = image({0x900: caller(0x900, 0x1200), 0x1200: [0xd2800020, RET], 0x300: [MOV, NOP, RET]})
        messages = []
        self.assertEqual([c[0] for c in resolve(moved, 0x2000, [self.rule], messages.append)], [0x908, 0x300])
        self.assertFalse([m for m in messages if m.startswith('[RED]')], messages)

    def test_one_changed_function_applies_none(self):
        changed = image({0x100: caller(0x100, 0x500), 0x500: [0xd2800020, RET], 0x800: [MOV, MOV, RET]})
        before, messages, outcomes = bytes(changed), [], []
        self.assertEqual(resolve(changed, 0x2000, [self.rule], messages.append, outcomes), [])
        self.assertEqual(bytes(changed), before)
        self.assertTrue(messages[0].startswith('[RED] ') and 'function 2 of 2' in messages[0] and 'the other 1 matched' in messages[0], messages)
        self.assertEqual(outcomes[0][:2], ('region', None))

    def test_alternative_implementations_of_a_patch(self):
        # a later build changed the bound's function: a second implementation of the same patch, which shares the first function
        later = image({0x100: caller(0x100, 0x500), 0x500: [0xd2800020, RET], 0x800: [NOP, MOV, NOP, RET]})
        alt = Image(later, 0x2000).patch_recipe([(0x108, 'e00317aa', 'e0124091', 'size'), (0x804, 'e00317aa', 'e0124091', 'bound')], 'region')
        for img, offs in ((self.original, [0x108, 0x800]), (later, [0x108, 0x804])):
            with self.subTest(offs=offs):
                outcomes = []
                self.assertEqual([c[0] for c in resolve(img, 0x2000, [self.rule, alt], lambda _: None, outcomes)], offs)
                self.assertEqual(outcomes[0][1], offs)

    def test_two_functions_cannot_match_one(self):
        rule = {'name': 'region', 'functions': [self.rule['functions'][1], dict(self.rule['functions'][1], cuts=[(4, '1f2003d5', '1f2003d5', 'again')])]}
        with self.assertRaisesRegex(MatchError, 'another one did'): Image(self.original, 0x2000).match_patch(rule)

    def test_malformed_patches_are_rejected(self):
        f = self.rule['functions'][0]
        for bad in ({'name': 'region', 'functions': [f]}, {'name': 'region', 'functions': [f, f], 'size': 4},
                    {'name': 'region', 'functions': [f, dict(f, name='other')]}, {'name': 'region', 'functions': [f, dict(f, cuts=[])]}):
            with self.assertRaises(ValueError): validate(bad)




class Registry(unittest.TestCase):
    def setUp(self):
        self.original = image({0x100: caller(0x100, 0x500), 0x500: [0xd2800020, RET]})
        self.rule = Image(self.original, 0x2000).recipe(0x108, 'e00317aa', 'e0124091', 'heap')
        source = TOOLS / 'patch_nso.py'; tree = ast.parse(source.read_text())
        start = next(i for i, n in enumerate(tree.body) if isinstance(n, ast.Assign) and ast.unparse(n.targets[0]) == 'PATCH_FILES')
        end = next(i for i, n in enumerate(tree.body[start:], start) if isinstance(n, ast.Assign) and ast.unparse(n.targets[0]) == 'KEYS')
        self.code = compile(ast.Module(body=tree.body[start:end], type_ignores=[]), str(source), 'exec')

    def rewrite(self, name='sdk', bid='00' * 16, extra=None, data=None):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); (root / 'tools').mkdir()
            cuts = {'ab' * 16: [(0x108, 'e00317aa', '1f2003d5', 'known')], 'cd' * 16: []}
            (root / 'tools/sdk_patches.py').write_text(f'CUTS = {cuts!r}\nMATCH_CUTS = {{"sdk": [{self.rule!r}]}}\n')
            env = dict(os.environ, DM_PATCHES_EXTRA='')
            if extra:
                path = root / 'extra.py'; path.write_text(f'CUTS = {{{bid!r}: {extra!r}}}\n')
                env['DM_PATCHES_EXTRA'] = str(path)
            img = bytearray(self.original if data is None else data)
            ns = dict(__file__=str(root / 'tools/patch_nso.py'), os=os, glob=glob, importlib=importlib,
                      d=bytes(0x40) + bytes.fromhex(bid), IMG=img, TEXT_END=0x2000, NAME=name)
            output = io.StringIO()
            with patch.dict(os.environ, env, clear=True), contextlib.redirect_stdout(output): exec(self.code, ns)
            return img, output.getvalue()

    def test_sdk_fallback_preserves_registered_and_empty_rows_and_module_scope(self):
        for module, bid, expected in (('sdk', '00' * 16, ADD), ('sdk', 'ab' * 16, NOP),
                                      ('sdk', 'cd' * 16, MOV), ('main', '00' * 16, MOV)):
            with self.subTest(module=module, build=bid):
                img, _ = self.rewrite(module, bid)
                self.assertEqual(struct.unpack_from('<I', img, 0x108)[0], expected)

    def test_extra_cuts_append_without_disabling_fallback(self):
        extra = [(0x114, '1f2003d5', 'c0035fd6', 'extra')]
        img, _ = self.rewrite(extra=extra)
        self.assertEqual(struct.unpack_from('<I', img, 0x108)[0], ADD)
        self.assertEqual(struct.unpack_from('<I', img, 0x114)[0], RET)
        img, _ = self.rewrite(bid='ab' * 16, extra=extra)
        self.assertEqual(struct.unpack_from('<I', img, 0x108)[0], NOP)
        self.assertEqual(struct.unpack_from('<I', img, 0x114)[0], RET)

    def test_extra_overlap_still_fails_original_byte_check(self):
        with self.assertRaisesRegex(AssertionError, 'cut original bytes mismatch'):
            self.rewrite(extra=[(0x108, 'e00317aa', '1f2003d5', 'overlap')])

    def test_unknown_function_is_reported_and_left_unchanged(self):
        data = image({0x100: [RET]})
        got, output = self.rewrite(data=data)
        self.assertEqual(got, data)
        self.assertIn("function patch 'heap' skipped", output)
        self.assertNotIn('[RED]', output)

    def test_unknown_sdk_rewrite_cache_tracks_rules_and_matcher(self):
        from porter.upstream import nsp
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); repo = root / 'repo'; tools = repo / 'dm/tools'; tools.mkdir(parents=True)
            for name in ('patch_nso.py', 'nso_code.py', 'nso_asm.py', 'nso_match.py', 'sdk_patches.py'):
                (tools / name).write_text('CUTS = {}\n')
            exefs = root / 'extract/ExeFS'; exefs.mkdir(parents=True)
            (exefs / 'sdk').write_bytes(b'NSO0' + bytes(0x4c))
            out = root / 'patched'; out.mkdir(); (out / 'sdk.patched.bin').write_bytes(b'output')
            g = SimpleNamespace(words=lambda _: ['sdk']); keys = []
            def reused(path, key): keys.append(key); return 'fixture'
            with patch.object(nsp, 'collect_nros', return_value={}), patch.object(nsp, 'PROG'), patch.object(nsp, 'reused', side_effect=reused):
                nsp.rewrite(sys.executable, repo, exefs, out, {}, root / 'logs', g)
                (tools / 'nso_match.py').write_text('# matcher changed\n')
                nsp.rewrite(sys.executable, repo, exefs, out, {}, root / 'logs', g)
                (tools / 'sdk_patches.py').write_text('CUTS = {}\n# rules changed\n')
                nsp.rewrite(sys.executable, repo, exefs, out, {}, root / 'logs', g)
            self.assertEqual(len(set(keys)), 3, 'unknown SDK rewrite reused across matcher or rule changes')


if __name__ == '__main__': unittest.main()
