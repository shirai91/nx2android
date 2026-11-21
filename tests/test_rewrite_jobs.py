"""Module rewrite scheduling, using synthetic NSOs and controlled child processes."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from porter.upstream import game, nsp


class RewriteJobs(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name); self.repo = self.root / 'repo'
        self.exe = self.root / 'game/ExeFS'; self.exe.mkdir(parents=True)
        self.modules = ['main', 'subsdk0', 'sdk']
        for name in self.modules: (self.exe / name).write_bytes(b'NSO0' + bytes(0x50))
        for name in ('patch_nso.py', 'nso_code.py', 'nso_asm.py', 'sdk_patches.py'):
            path = self.repo / 'dm/tools' / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text('# fixture\n')
        self.out = self.root / 'patched_test'; self.logs = self.root / 'logs'
        profile = self.root / 'profile'; profile.mkdir()
        (profile / 'game.tsv').write_text('modules\t' + ' '.join(self.modules) + '\n', encoding='utf-8')
        self.profile = game.Game(profile)
        self.children = []; self.active = set(); self.peak = 0; self.failing_module = None
        for patch in (mock.patch.object(nsp, 'PROG'), mock.patch.object(nsp.os, 'cpu_count', return_value=8),
                      mock.patch.object(nsp.subprocess, 'Popen', side_effect=self.launch), mock.patch.object(nsp.time, 'sleep')):
            patch.start(); self.addCleanup(patch.stop)

    def launch(self, command, **kwargs):
        self.assertEqual(command[:2], [sys.executable, str(self.repo / 'dm/tools/patch_nso.py')])
        name = command[-1]; self.assertEqual(command[2], str(self.exe / name))
        owner = self

        class Child:
            def __init__(self):
                self.name = name; self.returncode = None; self.polls = 0; self.terminated = False; self.waited = False
                self.log = kwargs['stdout']

            def poll(self):
                self.polls += 1
                # Leave a sibling running while the failed child is reported, exercising cancellation.
                if self.returncode is None and self.polls >= 2 and (owner.failing_module is None or name == owner.failing_module):
                    self.returncode = 7 if name == owner.failing_module else 0; owner.active.remove(self)
                    if self.returncode: self.log.write('synthetic rewrite failure\n'); self.log.flush()
                    else: (Path(command[3]) / f'{name}.patched.bin').write_bytes(name.encode())
                return self.returncode

            def terminate(self):
                self.terminated = True; self.returncode = -15; owner.active.remove(self)

            def wait(self):
                self.waited = True; return self.returncode

        child = Child(); self.children.append(child); self.active.add(child); self.peak = max(self.peak, len(self.active))
        return child

    def rewrite(self, jobs=None):
        env = {} if jobs is None else {'J': jobs}
        nsp.rewrite(sys.executable, self.repo, self.exe, self.out, env, self.logs, self.profile)

    def assert_outputs(self):
        self.assertEqual({p.name: p.read_bytes() for p in self.out.iterdir()}, {f'{name}.patched.bin': name.encode() for name in self.modules})
        self.assertFalse(self.active)
        self.assertTrue(all(child.log.closed for child in self.children))

    def test_one_job_serializes_all_modules(self):
        self.rewrite('1')
        self.assertEqual(self.peak, 1)
        self.assertEqual([child.name for child in self.children], self.modules)
        self.assert_outputs()

    def test_two_jobs_allow_overlap_without_exceeding_limit(self):
        self.rewrite('2')
        self.assertEqual(self.peak, 2)
        self.assert_outputs()

    def test_missing_limit_uses_cpu_fallback(self):
        with mock.patch.object(nsp.os, 'cpu_count', return_value=2): self.rewrite()
        self.assertEqual(self.peak, 2)
        self.assert_outputs()

    def test_changing_limit_reuses_completed_outputs(self):
        self.rewrite('2'); self.assert_outputs()
        key = self.out.with_name(self.out.name + '.key').read_bytes()
        self.rewrite('1')
        self.assertEqual(len(self.children), 3)
        self.assertEqual(self.out.with_name(self.out.name + '.key').read_bytes(), key)
        self.assert_outputs()

    def test_failure_stops_running_sibling_and_leaves_queued_module_unstarted(self):
        self.out.mkdir(); (self.out / 'keep').write_bytes(b'previous output')
        self.failing_module = 'main'
        with self.assertRaisesRegex(nsp.BuildError, 'exit code 7 on main'): self.rewrite('2')
        self.assertEqual([child.name for child in self.children], ['main', 'subsdk0'])
        sibling = self.children[1]; self.assertTrue(sibling.terminated and sibling.waited)
        self.assertFalse(self.active)
        self.assertTrue(all(child.log.closed for child in self.children))
        self.assertEqual((self.out / 'keep').read_bytes(), b'previous output')
        self.assertFalse(self.out.with_name(self.out.name + '.key').exists())

    def test_invalid_limit_fails_before_starting_or_replacing_outputs(self):
        self.out.mkdir(); (self.out / 'keep').write_bytes(b'previous output')
        for jobs in ('0', '-1', '', 'not-a-number'):
            with self.subTest(jobs=jobs), self.assertRaisesRegex(nsp.BuildError, 'positive integer'): self.rewrite(jobs)
            self.assertFalse(self.children)
            self.assertEqual((self.out / 'keep').read_bytes(), b'previous output')


if __name__ == '__main__': unittest.main()
