"""The compute warm-up: the bundle's shaders by default, the game's own with a switch.

A bundle holds many compute shaders (Fire Emblem: Three Houses: 147) and the driver's shader
compiler can die on one of them: the Adreno 750's (libllvm-qgl.so) faults (SIGSEGV, NULL+0x48) on
the game's compute shader 00d5d19d07c37895, a second and a half after the launch and before the
title screen, and the app's log has no crash report of it.

So the warm-up builds the shaders of the bundle, as it did before, or the ones the game has
dispatched with DM_VK_COMPUTE_DISPATCHED=1 (a profile's env row: Fire Emblem: Three Houses sets
it). The record of the dispatched shaders holds each key once, and a shader whose build crashed
the driver's compiler is written down before the build and skipped at every later start.
"""
from pathlib import Path
import unittest
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm/android'
GAMES = ROOT / 'nxnative/dm/games'
sys.path.insert(0, str(DM / 'tools'))
import settings_registry  # noqa: E402


class ComputeWarmup(unittest.TestCase):
    def test_the_bundle_s_shaders_are_built_by_default(self):
        src = csrc.unit(DM / 'gfx_vk_exec.c')
        self.assertIn('int nu = n;', src)                              # the bundle's own shaders, as before
        self.assertIn('nu = compute_used(used, 4096);', src)          # the game's own, with the switch
        self.assertIn('uint64_t key = dispatched ? used[i] : keys[i];', src)

    def test_the_dispatched_only_mode_is_a_profile_s_switch(self):
        reg = settings_registry.load()
        row = reg['DM_VK_COMPUTE_DISPATCHED']
        self.assertEqual(row.scope, 'profile')      # a game profile may set it
        self.assertEqual(row.type, 'bool')
        src = csrc.unit(DM / 'gfx_vk_exec.c')
        self.assertIn('getenv("DM_VK_COMPUTE_DISPATCHED")', src)
        # the profile that needs it (the Adreno 750's shader compiler dies on a shader the game never dispatches)
        fe3h = (GAMES / '010055D009F78000/android.tsv').read_text(encoding='utf-8')
        self.assertIn('env\tDM_VK_COMPUTE_DISPATCHED=1', fe3h)

    def test_the_record_of_the_dispatched_shaders_holds_each_key_once(self):
        src = csrc.unit(DM / 'gfx_vk_exec.c')
        self.assertIn('vk_compute_pipes.bin', src)                    # the record of the dispatched keys
        self.assertIn('compute_used(used,', src)                      # the warm-up reads it
        self.assertIn('if (nkeys < 0) nkeys = compute_used(keys, 4096);', src)   # the keys it holds
        self.assertIn('for (int i = 0; i < nkeys; i++) if (keys[i] == key)', src)  # a key in it is not written again

    def test_a_shader_that_crashes_the_compiler_is_skipped_at_every_start(self):
        src = csrc.unit(DM / 'gfx_vk_exec.c')
        self.assertIn('vk_compute_skip.txt', src)                     # where the culprits' keys are kept
        self.assertIn('int nbad = compute_skip_load(bad, 64);', src)   # the file holds a list, not one key
        self.assertIn('if (bad_key) continue;', src)                   # a key in it is skipped
        self.assertIn('compute_skip_save(bad, nbad);', src)            # written down before the build
        self.assertIn('if (added) { nbad--; compute_skip_save(bad, nbad); }', src)   # and taken out after a good one

    def test_the_warm_up_thread_has_a_stack_of_its_own(self):
        src = csrc.unit(DM / 'gfx_vk_exec.c')
        self.assertIn('pthread_attr_setstacksize(&a, 16 << 20);', src)   # the driver's compiler faults on a thread with little stack
        self.assertIn('crash_thread_init();', src)                        # and the report needs a signal stack of its own


if __name__ == '__main__':
    unittest.main()
