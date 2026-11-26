# The shader export's progress lines (nxnative/dm/android/tools/shader_vk.py): the porter turns them into the stage's own count, so a build shows how far the export has come
# instead of sitting on one line while the translation runs.
import sys, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
from porter.pipeline import shader_progress   # noqa: E402


class ShaderProgress(unittest.TestCase):
    def test_translation_line(self):
        self.assertEqual(shader_progress('progress: translating 66,230 shaders: 12500/66230 shaders (2 of 8 batches, 42s)'),
                         ('translating 66,230 shaders', 12500, 66230, '12,500 of 66,230 shaders (2 of 8 batches, 42s)'))

    def test_compression_line(self):
        self.assertEqual(shader_progress('progress: compressing 3,128 shaders: 3128/3128 shaders (9s)'),
                         ('compressing 3,128 shaders', 3128, 3128, '3,128 of 3,128 shaders (9s)'))

    def test_the_first_line_of_a_pass(self):   # no tail yet: the count alone
        self.assertEqual(shader_progress('progress: translating 66,230 shaders: 0/66230 shaders'),
                         ('translating 66,230 shaders', 0, 66230, '0 of 66,230 shaders'))

    def test_the_other_passes_have_their_own_step(self):
        for step in ('translating 1,200 shaders again', 'translating 48 geometry shaders'):
            self.assertEqual(shader_progress(f'progress: {step}: 1/2 shaders')[0], step)

    def test_other_lines_are_not_progress(self):
        for line in ('translating 66230 shaders in 8 batches ...', 'progress: something else', '[RED] a shader failed', '', 'progress: translating shaders'):
            self.assertIsNone(shader_progress(line), line)


if __name__ == '__main__':
    unittest.main()
