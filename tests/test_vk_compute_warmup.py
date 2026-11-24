"""The compute warm-up builds only the shaders the game dispatched.

A bundle holds many compute shaders (Fire Emblem: Three Houses: 147) and the driver's shader
compiler can die on one of them: the Adreno 750's (libllvm-qgl.so) faults (SIGSEGV, NULL+0x48) on
the game's compute shader 00d5d19d07c37895, a second and a half after the launch and before the
title screen, and the app's log has no crash report of it. So the warm-up builds the shaders the
game has dispatched, the key of the one that crashed is written down before its build, and the next
run skips it.
"""
from pathlib import Path
import unittest
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm/android'


class ComputeWarmup(unittest.TestCase):
    def test_only_the_dispatched_shaders_are_built(self):
        src = csrc.unit(DM / 'gfx_vk_exec.c')
        self.assertIn('vk_compute_pipes.bin', src)           # the record of the dispatched keys
        self.assertIn('compute_used(used,', src)             # the warm-up reads it
        self.assertIn('for (int i = 0; i < nu; i++)', src)   # and builds those, not the bundle's
        self.assertIn('if (key == skip) continue;', src)    # the one that crashed last time is skipped
        self.assertIn('vk_compute_skip.txt', src)            # where the culprit's key is kept
        self.assertIn('compute_skip_set(key);', src)         # written down before the build, not after

    def test_the_warm_up_thread_has_a_stack_of_its_own(self):
        src = csrc.unit(DM / 'gfx_vk_exec.c')
        self.assertIn('pthread_attr_setstacksize(&a, 16 << 20);', src)   # the driver's compiler faults on a thread with little stack
        self.assertIn('crash_thread_init();', src)                        # and the report needs a signal stack of its own


if __name__ == '__main__':
    unittest.main()
