"""The rewriter's two memory-ordering rules (nxnative's, ported): every exclusive access gets
acquire/release ordering, and the Game Freak per-frame rendezvous completer gets a dmb ish
between its plain flag store and its state-word load.
"""
import re
import struct
import unittest
from pathlib import Path

REWRITER = Path(__file__).resolve().parents[1] / 'nxnative/dm/tools/patch_nso.py'


def load(name, stop):
    """One function out of patch_nso.py, without the CLI driver that loads game files (stop = the
    first top-level statement after it)."""
    source = REWRITER.read_text(encoding='utf-8')
    start = source.index(f'def {name}')
    end = source.index(stop, start)
    scope = dict(re=re, struct=struct)
    exec(compile(source[start:end], str(REWRITER), 'exec'), scope)
    return scope


def words(*ws):
    return bytearray(struct.pack('<' + 'I' * len(ws), *ws))


class ExclusiveOrdering(unittest.TestCase):
    def test_ldxr_and_stxr_get_the_ordered_form(self):
        img = words(0xd503201f, 0x885f7d09, 0xd503201f, 0x88097d00)   # ldxr w9,[x8]; stxr w9,x0,[x8]
        scope = load('exclusive_order_sites', 'ORDERED = set(')
        sites = scope['exclusive_order_sites'](img, [(0, len(img))])
        self.assertEqual(sites, [4, 12])
        for o in sites:
            w = struct.unpack_from('<I', img, o)[0]
            struct.pack_into('<I', img, o, w | 0x8000)               # what patch_nso does
        self.assertEqual(struct.unpack_from('<I', img, 4)[0], 0x885ffd09)    # ldaxr
        self.assertEqual(struct.unpack_from('<I', img, 12)[0], 0x88097d00 | 0x8000)   # stlxr

    def test_already_ordered_and_cas_are_left_alone(self):
        img = words(0x885ffd09, 0x88097d00 | 0x8000, 0xc8dffd09, 0xc8c97d09)   # ldaxr; stlxr; cas/casa
        scope = load('exclusive_order_sites', 'ORDERED = set(')
        self.assertEqual(scope['exclusive_order_sites'](img, [(0, len(img))]), [])


TRINITY = (0xa9be7bfd, 0xa9014ff4, 0x910003fd, 0xf9400008, 0x52800029, 0x39000109, 0x39400509,
           0x34000009, 0xf9400114, 0x91000289, 0x88dffd28, 0x7100091f, 0x54000001)


class TrinityCompleter(unittest.TestCase):
    def test_the_frame_is_found_and_the_dmb_lands_after_the_flag_store(self):
        img = words(0xd503201f, *TRINITY, 0xd503201f)
        scope = load('trinity_completer_sites', 'TRINITY = trinity_completer_sites(')
        self.assertEqual(scope['trinity_completer_sites'](img, [(0, len(img))]), [4])
        load('trinity_completer_barrier', 'TRINITY = trinity_completer_sites(')['trinity_completer_barrier'](img, 4)
        after = struct.unpack_from('<4I', img, 12)
        self.assertEqual(after, (TRINITY[3], TRINITY[4], TRINITY[5], 0xd5033bbf))   # ldr, mov, strb, dmb ish
        self.assertEqual(struct.unpack_from('<I', img, 28)[0], TRINITY[6])           # the ldrb follows it

    def test_a_near_miss_is_not_matched(self):
        img = words(0xd503201f, *(TRINITY[:4] + (0x52800028,) + TRINITY[5:]), 0xd503201f)   # mov w8,#1, not w9
        scope = load('trinity_completer_sites', 'TRINITY = trinity_completer_sites(')
        self.assertEqual(scope['trinity_completer_sites'](img, [(0, len(img))]), [])


if __name__ == '__main__':
    unittest.main()
