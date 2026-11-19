# README.md's games table and docs/PORT_CHECKLIST.md's marks and count are made from the profiles (docs/gen_tables.py): a pull request that adds a game edits its
# android.tsv and runs the script; this test fails when the files are not what the profiles make.
import subprocess, sys, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


class DocsTables(unittest.TestCase):
    def test_tables_match_the_profiles(self):
        r = subprocess.run([sys.executable, str(REPO / 'docs/gen_tables.py'), '--check'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_every_readme_image_exists(self):
        sys.path.insert(0, str(REPO / 'docs')); import gen_tables
        for cell in gen_tables.cells():
            img = cell.split('](', 1)[1].split(')', 1)[0]
            self.assertTrue((REPO / img).is_file(), img)


if __name__ == '__main__':
    unittest.main()
