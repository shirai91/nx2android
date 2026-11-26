"""The crash handlers are put back where the Vulkan driver reset them.

The Vulkan driver resets the app's signal handlers to the default action right after the
device is made, and again when it loads its shader compiler with the first pipeline build.
With the default action in place a fault is a silent kill: the process dies with no crash
report at all, which is what the Fire Emblem: Three Houses case saw (the run ended in a
silent SIGSEGV, no CRASH line in the log).

So the host puts its handlers back at those two points, and checks again that they are its
own. The function that does it puts the sigactions back and nothing else: crash_install()
also arms the watchdog and starts its thread, and calling it again gave the run three
watchdogs. A handler the driver installed over ours is left alone (a library may rely on
its own), as crash_handlers_check does.
"""
from pathlib import Path
import unittest
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm/android'
HOST = ROOT / 'nxnative/dm/host'


class CrashHandlers(unittest.TestCase):
    def test_the_handlers_are_put_back_after_the_device_and_the_first_build(self):
        core = (DM / 'gfx_vk_core.c').read_text(encoding='utf-8')
        self.assertIn('the crash handlers were put back after the Vulkan device was made', core)   # after the device was made
        self.assertIn('the crash handlers were put back after the first pipeline build', core)   # and after the first pipeline build
        self.assertIn('void vk_crash_handlers_after_build(void)', core)   # the second one happens once
        header = (DM / 'gfx_vk.h').read_text(encoding='utf-8')
        self.assertIn('void vk_crash_handlers_after_build(void);', header)
        exec_src = csrc.unit(DM / 'gfx_vk_exec.c')
        self.assertIn('vk_crash_handlers_after_build();', exec_src)   # the graphics and the compute build

    def test_the_restore_puts_the_sigactions_back_and_nothing_else(self):
        crash = (HOST / 'crash.c').read_text(encoding='utf-8')
        start = crash.index('int crash_handlers_restore(void) {')
        body = crash[start:crash.index('\n}', start) + 2]
        self.assertIn('sigaction(sigs[i], &sa, 0);', body)   # the handlers are ours again
        self.assertNotIn('alarm(', body)                      # the watchdog's timer is not re-armed
        self.assertNotIn('watchdog_main', body)               # and no second watchdog thread is started
        self.assertNotIn('pthread_create', body)

    def test_a_library_s_own_handler_is_left_alone(self):
        crash = (HOST / 'crash.c').read_text(encoding='utf-8')
        start = crash.index('int crash_handlers_restore(void) {')
        body = crash[start:crash.index('\n}', start) + 2]
        self.assertIn('o.sa_handler != SIG_DFL', body)   # only the default action is replaced: a library's own handler stays

    def test_the_crash_report_s_first_line_goes_out_at_once(self):
        crash = (HOST / 'crash.c').read_text(encoding='utf-8')
        start = crash.index('fprintf(stderr, "[dm] CRASH sig=%d", sig);')
        self.assertIn('fflush(stderr);', crash[start:start + 200])   # the line has no newline of its own


if __name__ == '__main__':
    unittest.main()
