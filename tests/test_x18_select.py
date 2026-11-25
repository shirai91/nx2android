"""patch_nso.py site_select: a csel with one x18 source is split on its flags, and its x18 arm loads K18 through the destination and a dead temporary, so the guest stack
below sp is never written (Sunshine's pitch resampler keeps its phase table live below sp; site_plain's save at sp-16 corrupted it). Without game files.

Optional dependency: unicorn, which runs the blocks."""
import ast, struct, unittest

try: import test_x18_sysreg as slots   # discover tests
except ImportError: from tests import test_x18_sysreg as slots

SITE, PATCH, KEY, TSD, TP = slots.SITE, slots.PATCH_BASE, slots.KEY_PAGE, slots.TSD, slots.TP
K18 = slots.SLOT['K18']
asm = slots.nso_asm
scope = None
if slots.scope is not None:
    scope = dict(slots.scope, patch=bytearray(), fix=[])
    tree = ast.parse((slots.TOOLS / 'patch_nso.py').read_text(encoding='utf-8'))
    functions = {'select_x18', 'select_scratch', 'site_select', 'here', 'emit', 'emit_tp', 'emit_key_addr', 'emit_slot_addr', 'dis', 'regset', 'num', 'regs_in'}
    nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in functions
             or isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id in ('SELECT_READS', 'SELECT_WRITES') for t in n.targets)]
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(slots.TOOLS / 'patch_nso.py'), 'exec'), scope)   # the helpers use this scope's patch buffer


def select(rd, rn, rm, wide):   # csel Rd, Rn, Rm, gt
    return 0x1A80C000 | (wide << 31) | (rm << 16) | (rn << 5) | rd


def emitted(word, temp, target):
    ins = scope['dis'](word, SITE)
    scope['patch'].clear(); scope['fix'].clear(); scope['TARGET'] = target
    scope['site_select'](ins, scope['select_x18'](ins), temp)
    return bytes(scope['patch'])


@unittest.skipIf(scope is None, 'capstone is not installed')
class Shape(unittest.TestCase):
    def test_only_csel_with_one_x18_source(self):
        dis = scope['dis']
        self.assertEqual(scope['select_x18'](dis(select(11, 18, 12, 1), SITE)), (11, 12, 0xC ^ 1, True))   # picks x12 when not gt
        self.assertEqual(scope['select_x18'](dis(select(11, 12, 18, 0), SITE)), (11, 12, 0xC, False))
        for word in (select(11, 18, 18, 1), select(18, 18, 12, 1), select(11, 12, 13, 1), select(11, 18, 12, 1) | 0x400):   # both x18, x18 written, none, csinc
            with self.subTest(word=hex(word)): self.assertIsNone(scope['select_x18'](dis(word, SITE)))

    def test_no_stack_access_and_no_x18(self):
        for target in slots.TARGETS:
            with self.subTest(target=target):
                ins = list(slots.md.disasm(emitted(select(11, 18, 12, 1), 9, target), PATCH))
                text = [f'{i.mnemonic} {i.op_str}' for i in ins]
                self.assertFalse([t for t in text if 'sp' in t.split(' ', 1)[-1] or 'x18' in t or 'w18' in t], text)
                self.assertTrue(text[0].startswith('b.le'), text)   # the other arm, on the inverted condition


@unittest.skipIf(scope is None, 'capstone is not installed')
class ScratchProof(unittest.TestCase):
    def scratch(self, code):
        start = SITE + 4
        ins = {start + 4 * n: scope['dis'](word, start + 4 * n) for n, word in enumerate(code)}
        return scope['select_scratch'](SITE, ins, start + len(code) * 4, 11)

    def test_write_on_both_paths(self):
        branch = asm.BEQ(SITE + 4, SITE + 16)
        escape = asm.B(SITE + 12, SITE + 0x100)
        kill = asm.MOVX(9, 0)
        self.assertEqual(self.scratch([branch, kill, escape, kill]), 9)
        self.assertIsNone(self.scratch([branch, kill, escape, asm.ADDI(9, 9, 1)]))

    def test_load_address_is_read_before_overwrite(self):
        self.assertIsNone(self.scratch([asm.LDRX(9, 9)]))

    def test_call_may_read_any_unproven_register(self):
        bl = 0x94000000 | ((0x100 - 4) >> 2)   # bl SITE + 0x100
        self.assertIsNone(self.scratch([bl, 0xD65F03C0]))

    def test_return_ends_every_temporary(self):
        self.assertEqual(self.scratch([0xD65F03C0]), 9)   # ret: x9–x17 are dead, the lowest is taken


@unittest.skipIf(scope is None or slots.uc is None, 'optional ARM64 execution dependency: unicorn')
class Run(unittest.TestCase):
    def test_select_with_unmapped_stack(self):
        uc, arm = slots.uc, slots.arm
        for target in slots.TARGETS:
            for wide in (False, True):
                for rn, rm in ((11, 18), (18, 12), (18, 31)):
                    for gt, nzcv in ((True, 0), (False, 1 << 30)):
                        with self.subTest(target=target, wide=wide, rn=rn, rm=rm, gt=gt):
                            mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
                            for base in (0, PATCH, KEY, TSD, TP): mu.mem_map(base, 0x10000)
                            mu.mem_write(PATCH, emitted(select(11, rn, rm, wide), 9, target))
                            mu.mem_write(KEY, struct.pack('<Q', K18))
                            value18 = 0xF00D00005678ABCD
                            mu.mem_write(TSD + K18, struct.pack('<Q', value18))
                            if target == 'android': mu.mem_write(TP + 16, struct.pack('<Q', TSD)); mu.reg_write(arm.UC_ARM64_REG_TPIDR_EL0, TP)
                            else: mu.reg_write(arm.UC_ARM64_REG_TPIDRRO_EL0, TSD)
                            mu.reg_write(arm.UC_ARM64_REG_SP, 0x10000000)   # unmapped: any stack access faults
                            mu.reg_write(arm.UC_ARM64_REG_NZCV, nzcv)
                            values = {9: 9, 11: 0xBAAC000012345678, 12: 0xCDEF0000CAFE0123, 18: 0xDEAD}
                            for reg, value in values.items(): mu.reg_write(arm.UC_ARM64_REG_X0 + reg, value)
                            mu.emu_start(PATCH, SITE + 4)
                            chosen = rn if gt else rm
                            expected = value18 if chosen == 18 else values.get(chosen, 0)
                            self.assertEqual(mu.reg_read(arm.UC_ARM64_REG_X11), expected if wide else expected & 0xFFFFFFFF)
                            self.assertEqual(mu.reg_read(arm.UC_ARM64_REG_SP), 0x10000000)
                            self.assertEqual(mu.reg_read(arm.UC_ARM64_REG_NZCV), nzcv)
                            self.assertEqual(mu.reg_read(arm.UC_ARM64_REG_X18), values[18])
                            if chosen != 18: self.assertEqual(mu.reg_read(arm.UC_ARM64_REG_X9), values[9])


if __name__ == '__main__': unittest.main()
