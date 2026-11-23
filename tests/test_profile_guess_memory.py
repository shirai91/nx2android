"""Provisional address-space partitions must leave room for the game's startup heap."""
import sys, unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'nxnative/dm/games'))
import profile_guess as pg


class ProvisionalMemory(unittest.TestCase):
    def guess(self, markers, sysres=80 << 20):
        facts = {'mods': {'main': {'imports': {'nvnBootstrapLoader', pg.HEAP_API}, 'bss': 0, 'far': False}},
                 'romfs': {'root': set(), 'exts': {'.pak': 1}}, 'sysres': sysres, 'main_markers': markers}
        return pg.guess(facts, hooks=set())

    def heap_capacity(self, keys):
        return int(keys['mem_reserve'][0], 0) - 0x10000000 - int(keys['mem_alias'][0], 0) - int(keys['mem_stack'][0], 0)

    def test_unreal_virtual_address_memory_keeps_a_startup_heap(self):
        for marker in ('Unreal', 'Unreal 4'):
            with self.subTest(marker=marker):
                keys = self.guess({marker})
                self.assertGreaterEqual(self.heap_capacity(keys), 6 << 20)
                self.assertGreaterEqual(int(keys['mem_alias'][0], 0), 62 << 30)

    def test_unreal_without_virtual_address_memory_keeps_its_heap_layout(self):
        keys = self.guess({'Unreal 4'}, sysres=0)
        self.assertGreater(self.heap_capacity(keys), 0)
        self.assertLess(int(keys['mem_alias'][0], 0), 1 << 30)

    def test_other_engines_keep_the_smaller_partition(self):
        keys = self.guess(set())
        self.assertGreater(self.heap_capacity(keys), 0)
        self.assertLess(int(keys['mem_alias'][0], 0), 8 << 30)


if __name__ == '__main__': unittest.main()
