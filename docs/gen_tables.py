#!/usr/bin/env python3
"""The parts of README.md and docs/PORT_CHECKLIST.md that come from the game profiles, made from them (pull requests that add a game edit only its profile):
  README.md, Games: the table of games, one cell for each profile whose android.tsv has readme_* rows (readme_image, readme_tag, readme_note; readme_name when
                    the table names the game otherwise than game.tsv), in name order, two to a row.
  docs/PORT_CHECKLIST.md: each listed game is checked when its profile has an android.tsv, and the count line follows.
    python docs/gen_tables.py           write both files
    python docs/gen_tables.py --check   exit 1 when a file is not what the profiles make (tests/test_docs_tables.py runs it)
"""
import re, sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'nxnative/dm/android/tools'))
import android_profile   # noqa: E402

BEGIN = '<!-- games table: made by docs/gen_tables.py from the readme_* rows of each android.tsv; edit those, not this table -->'
END = '<!-- end of the games table -->'


def cells():
    out = []
    for g, p in android_profile.all_profiles():
        if not p or not p.get('readme_tag'): continue
        name = p.get('readme_name') or g.get('name', g.id)
        out.append((name, f"![{name}]({p.get('readme_image')})<br>{name}<br><sub>`{p.get('readme_tag')}` · {p.get('readme_note')}</sub>"))
    out.sort(key=lambda c: c[0].casefold())
    return [c for _, c in out]


def readme_table():
    cs = cells(); rows = ['| | |', '|:---:|:---:|']
    for i in range(0, len(cs), 2):
        pair = cs[i:i + 2]
        rows.append('| ' + ' | '.join(pair) + (' |' if len(pair) == 2 else ' | |'))
    return '\n'.join(rows)


def readme(text):
    if BEGIN not in text or END not in text: raise SystemExit(f'README.md: the games table markers are missing ({BEGIN!r} ... {END!r})')
    a = text.index(BEGIN) + len(BEGIN); b = text.index(END)
    return text[:a] + '\n' + readme_table() + '\n' + text[b:]


def checklist(text):
    games = REPO / 'nxnative/dm/games'
    def mark(m):
        return f"- [{'x' if (games / m.group(2) / 'android.tsv').is_file() else ' '}] {m.group(1)}"
    text = re.sub(r'^- \[[ x]\] (.*? · `(0100[0-9A-F]{12})`)', mark, text, flags=re.M)
    done = len(re.findall(r'^- \[x\] ', text, flags=re.M)); total = len(re.findall(r'^- \[[ x]\] ', text, flags=re.M))
    return re.sub(r'^\*\*\d+ / \d+ ported\*\*$', f'**{done} / {total} ported**', text, count=1, flags=re.M)


def main(argv):
    check = '--check' in argv; bad = []
    for path, make in ((REPO / 'README.md', readme), (REPO / 'docs/PORT_CHECKLIST.md', checklist)):
        old = path.read_text(encoding='utf-8'); new = make(old)
        if new == old: continue
        if check: bad.append(str(path.relative_to(REPO)))
        else: path.write_text(new, encoding='utf-8', newline='\n'); print(f'wrote {path.relative_to(REPO)}')
    if bad: print('not what the profiles make: ' + ', '.join(bad) + ' (run python docs/gen_tables.py)'); return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
