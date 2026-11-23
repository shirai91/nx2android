# README.md's games list, each listed game's page (nxnative/dm/games/<title ID>/README.md: its facts block) and docs/PORT_CHECKLIST.md's marks and count are made
# from the profiles (docs/gen_tables.py): a pull request that adds a game edits its android.tsv and its page, and runs the script; this test fails when the files
# are not what the profiles make, a page lacks a required section (docs/GAME_README_TEMPLATE.md) or a relative link in them goes nowhere.
import re, subprocess, sys, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'docs'))
import gen_tables   # noqa: E402


class DocsTables(unittest.TestCase):
    def test_tables_match_the_profiles(self):
        r = subprocess.run([sys.executable, str(REPO / 'docs/gen_tables.py'), '--check'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_every_readme_image_exists(self):
        for _, _, p in gen_tables.listed():
            self.assertTrue((REPO / p.get('readme_image')).is_file(), p.get('readme_image'))

    def test_every_listed_game_has_a_page(self):
        for name, g, _ in gen_tables.listed():
            self.assertTrue((g.dir / 'README.md').is_file(), f'{name}: no {gen_tables.page(g)}')

    def test_relative_links_resolve(self):
        pages = [REPO / 'README.md'] + [g.dir / 'README.md' for _, g, _ in gen_tables.listed()]
        for page in pages:
            text = re.sub(r'```.*?```', '', page.read_text(encoding='utf-8'), flags=re.S)
            for target in re.findall(r'\]\(([^)\s]+)\)', text):
                if re.match(r'[a-z]+:|#', target): continue
                path = target.split('#', 1)[0]
                with self.subTest(page=str(page.relative_to(REPO)), link=target):
                    self.assertTrue((page.parent / path).exists(), f'{page.relative_to(REPO)}: {target} goes nowhere')


if __name__ == '__main__':
    unittest.main()
