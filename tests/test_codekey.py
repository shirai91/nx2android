# porter/codekey.py: the translator's source key changes with its code, not with its comments or whitespace (a comment-only change keeps every game's shader bundle).
import sys, tempfile, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
from porter import codekey   # noqa: E402

SRC = r'''// a comment "with quotes"
namespace A {   /* block
   comment */
    class B {
        string u = "http://example/*not*/";   // trailing
        string v = @"C:\dir\""quoted"" // still string";
        string w = $"{x}// in an interpolation";
        char q = '"'; char s = '/'; char e = '\'';
        int Sum(int a,   int b) => a + b;
    }
}
'''


class CodeKey(unittest.TestCase):
    def test_comments_and_whitespace_do_not_count(self):
        code = codekey.cs_code(SRC)
        for lit in ('"http://example/*not*/"', '@"C:\\dir\\""quoted"" // still string"', '$"{x}// in an interpolation"', "'\"'", "'/'", "'\\''"):
            self.assertIn(lit, code)
        self.assertNotIn('a comment', code); self.assertNotIn('block', code); self.assertNotIn('trailing', code)
        self.assertEqual(codekey.cs_code(SRC.replace('// trailing', '// another note').replace('int a,   int b', 'int a, int b') + '\n\n'), code)
        self.assertNotEqual(codekey.cs_code(SRC.replace('a + b', 'a - b')), code)
        self.assertNotEqual(codekey.cs_code(SRC.replace('/*not*/', '/*yes*/')), code)   # inside a string it is code

    def test_file_key(self):
        with tempfile.TemporaryDirectory() as t:
            a, b, p = Path(t) / 'a.cs', Path(t) / 'b.cs', Path(t) / 'x.csproj'
            a.write_text(SRC, encoding='utf-8'); b.write_text('\ufeff' + SRC.replace('// trailing', ''), encoding='utf-8'); p.write_text('<Project/>', encoding='utf-8')
            self.assertEqual(codekey.file_key(a), codekey.file_key(b))
            k = codekey.file_key(p); p.write_text('<Project />', encoding='utf-8'); self.assertNotEqual(codekey.file_key(p), k)   # project files: their bytes

    def test_every_translator_source_keys(self):
        files = sorted((REPO / 'nxnative/dm/third_party/ryujinx').rglob('*.cs')) + sorted((REPO / 'nxnative/dm/gfx/tools/dmsh_xlate').glob('*.cs'))
        self.assertGreater(len(files), 300)
        for f in files: self.assertEqual(len(codekey.file_key(f)), 64)


if __name__ == '__main__':
    unittest.main()
