"""Spin analysis must release decoded instructions from rejected candidates."""
import gc
from pathlib import Path
import re
import struct
import unittest
import weakref

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM, ARM64_OP_REG

REWRITER = Path(__file__).resolve().parents[1] / 'nxnative/dm/tools/patch_nso.py'
NOP = 0xd503201f


class TrackedDisassembler:
    """Observe live instruction objects without keeping them alive ourselves."""
    def __init__(self):
        self.md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); self.md.detail = True
        self.live = weakref.WeakSet()

    def disasm(self, *args):
        for ins in self.md.disasm(*args):
            self.live.add(ins)
            yield ins


def load(words):
    # Execute the real spin analysis, without the CLI driver that loads game files.
    source = REWRITER.read_text(encoding='utf-8')
    scope = dict(re=re, struct=struct, ARM64_OP_IMM=ARM64_OP_IMM,
                 ARM64_OP_MEM=ARM64_OP_MEM, ARM64_OP_REG=ARM64_OP_REG,
                 SPIN_WIN=512, DATA_BLOCKS=[], kinds={}, done=set(), obs=[], cuts=[],
                 IMG=bytearray(struct.pack('<' + 'I' * len(words), *words)), md=TrackedDisassembler())
    start, end = source.index('SPIN_PATHS, SPIN_STEPS'), source.index('spin_regions =')
    exec(compile(source[start:end], str(REWRITER), 'exec'), scope)
    return scope


class SpinMemory(unittest.TestCase):
    def test_rejected_loops_do_not_accumulate_decoded_instructions(self):
        # add w0,w0,#1; 14 nops; cbnz w0,-60: a counted loop, not a spin wait.
        words = [0x11000400] + [NOP] * 14 + [0x35fffe20]
        scope = load(words * 128)
        for head in range(0, len(scope['IMG']), 64):
            self.assertIsNone(scope['spin_region'](head, [head + 60]))
        gc.collect()
        self.assertLessEqual(len(scope['md'].live), 129,
                             'rejected candidates must not retain module-wide Capstone details')

    def test_saved_region_survives_analysis_of_later_candidates(self):
        # ldr w0,[x1]; cbnz w0,-4: only another thread can end this poll.
        poll = [0xb9400020, 0x35ffffe0]
        counted = [0x11000400] + [NOP] * 14 + [0x35fffe20]
        scope = load(poll + counted * 128)
        region = scope['spin_region'](0, [4])
        self.assertIsNotNone(region)
        for head in range(8, len(scope['IMG']), 64):
            self.assertIsNone(scope['spin_region'](head, [head + 60]))
        gc.collect()
        self.assertEqual(region['R'], [0, 4])
        self.assertEqual(region['valid'], {4})
        self.assertEqual([(n['ins'][0].mnemonic, bytes(n['ins'][0].bytes))
                          for n in region['nodes'].values()],
                         [('ldr', bytes.fromhex('200040b9')), ('cbnz', bytes.fromhex('e0ffff35'))])
        self.assertLessEqual(len(scope['md'].live), 131)

    def test_overlapping_candidates_see_the_same_unmodified_image(self):
        # Two backward branches to different heads share one polling load.
        scope = load([NOP, 0xb9400020, 0x35ffffc0, 0x35ffffc2])
        outer = scope['spin_region'](0, [8])
        inner = scope['spin_region'](4, [12])
        self.assertEqual(outer['R'], [0, 4, 8])
        self.assertEqual(outer['valid'], {8})
        self.assertEqual(inner['R'], [4, 8, 12])
        self.assertEqual(inner['valid'], {12})
        self.assertEqual(bytes(outer['nodes'][8]['ins'][0].bytes), bytes.fromhex('c0ffff35'))

    def test_maximum_window_keeps_all_instructions_for_accepted_region(self):
        # 512-byte backward branch: 129 instructions including both endpoints.
        scope = load([0xb9400020] + [NOP] * 127 + [0x35fff000])
        region = scope['spin_region'](0, [512])
        self.assertEqual(region['R'], list(range(0, 516, 4)))
        self.assertEqual(region['valid'], {512})

    def test_overlapping_accepted_regions_share_decoded_instructions(self):
        # 64 overlapping polls, each ending with cbnz w0,.-256. All graphs
        # await emission together, so clearing the whole cache duplicates them.
        scope = load([NOP] * 64 + [0x35fff800] * 64)
        regions = [scope['spin_region'](head, [head + 256]) for head in range(0, 256, 4)]
        for head, region in zip(range(0, 256, 4), regions):
            self.assertEqual(region['R'], list(range(head, head + 260, 4)))
            self.assertEqual(region['valid'], {head + 256})
        gc.collect()
        self.assertEqual(len(scope['md'].live), 128,
                         'overlapping graphs should share one decoding per instruction')


if __name__ == '__main__': unittest.main()
