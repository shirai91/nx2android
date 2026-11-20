#!/usr/bin/env python3
"""The parts of the docs that come from the game profiles, made from them (pull requests that add a game edit only its profile folder):
  README.md, Games: one cell for each profile whose android.tsv has readme_* rows (readme_image, readme_tag; readme_name when the page names the game
                    otherwise than game.tsv): the game's picture and name, both a link to its page, in name order, three to a row.
  nxnative/dm/games/<title ID>/README.md: the game's own page. Its first block (the title, the picture, the facts table and what the profile sets) is made
                    from android.tsv; the sections under it are written by hand (docs/GAME_README_TEMPLATE.md has the headings, REQUIRED lists them).
  docs/PORT_CHECKLIST.md: each listed game is checked when its profile has an android.tsv, and the count line follows.
    python docs/gen_tables.py           write the files
    python docs/gen_tables.py --check   exit 1 when a file is not what the profiles make, or a game page lacks a required section (tests/test_docs_tables.py runs it)
"""
import re, sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'nxnative/dm/android/tools'))
import android_profile    # noqa: E402
import settings_registry  # noqa: E402

BEGIN = '<!-- games table: made by docs/gen_tables.py from the readme_* rows of each android.tsv; edit those, not this table -->'
END = '<!-- end of the games table -->'
FACTS_BEGIN = '<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->'
FACTS_END = '<!-- end of the game facts -->'
REQUIRED = ('## Game version', '## Frame rate', '## Tested devices', '## Options', '## Quirks and known issues')
TEMPLATE = REPO / 'docs/GAME_README_TEMPLATE.md'
PER_ROW = 3
TO_REPO = '../../../../'   # from nxnative/dm/games/<title ID>/ to the repository root
MENU = {'overlay': 'on-screen gamepad shown (1) or hidden (0)', 'rumble': 'rumble on (1) or off (0)', 'docked': 'docked mode (1) or handheld mode (0)',
        'fps': 'frame-rate counter shown (1) or hidden (0)', 'opacity': 'on-screen gamepad opacity, percent'}
SYSDATA = {'0100000000000802': 'MiiModel', '0100000000000810': 'shared font (Nintendo extension)', '0100000000000811': 'shared font (standard)',
           '0100000000000812': 'shared font (Korean)', '0100000000000813': 'shared font (traditional Chinese)', '0100000000000814': 'shared font (simplified Chinese)'}


def listed():   # [(name, game, android profile)]: the profiles with a games-table cell, in name order
    out = [(p.get('readme_name') or g.get('name', g.id), g, p) for g, p in android_profile.all_profiles() if p and p.get('readme_tag')]
    return sorted(out, key=lambda c: c[0].casefold())


def page(g):   # the game's page, relative to the repository root
    return f'nxnative/dm/games/{g.id}/README.md'


def cells():
    return [f"[![{n}]({p.get('readme_image')})]({page(g)})<br>[{n}]({page(g)})" for n, g, p in listed()]


def readme_table():
    cs = cells(); rows = ['|' + ' |' * PER_ROW, '|' + ':---:|' * PER_ROW]
    for i in range(0, len(cs), PER_ROW):
        part = cs[i:i + PER_ROW]
        rows.append('| ' + ' | '.join(part + [''] * (PER_ROW - len(part))) + ' |')
    return '\n'.join(rows)


def replace(text, begin, end, body, where):
    if begin not in text or end not in text: raise SystemExit(f'{where}: the markers are missing ({begin!r} ... {end!r})')
    a = text.index(begin) + len(begin); b = text.index(end)
    return text[:a] + '\n' + body + '\n' + text[b:]


def readme(text):
    return replace(text, BEGIN, END, readme_table(), 'README.md')


def cell(s): return s.replace('|', '\\|')


def short(doc, most=160):   # a switch's doc (nxnative/dm/settings.tsv) to its first sentence, for a table cell; `show <name>` there gives the rest
    s = re.split(r'(?<=[a-z0-9)`])\. ', doc, maxsplit=1)[0].rstrip('.')
    return s if len(s) <= most else s[:most].rsplit(' ', 1)[0] + ' …'


def facts(name, g, p):
    """The made block at the top of a game's page"""
    reg = settings_registry.load()
    rows = [('Status', f'**{p.status}**'), ('Title ID', f'`{g.id}`'), ('Tested with nx2android', f"`{p.get('readme_tag')}`"),
            ('Tested build and device', p.tested or '-'), ('App package', f'`{p.package}`')]
    if p.vulkan_driver == 'turnip': rows.append(('Vulkan driver', 'Turnip: the port packs Mesa\'s Turnip driver when the porter finds it (`vulkan_driver turnip`)'))
    needs = []
    if 'bcn' in p.requires: needs.append('a GPU that reads BC-compressed textures (Adreno, Xclipse; not Mali or PowerVR)')
    if p.sysdata: needs.append('your console\'s firmware dump (`--firmware`, the GUI\'s Firmware field) for ' + ', '.join(SYSDATA.get(f'{t:016X}', f'{t:016X}') for t in p.sysdata))
    if needs: rows.append(('Needs', '; '.join(needs)))
    out = [f'# {name}', '', f"![{name}]({TO_REPO}{p.get('readme_image')})", '', '| | |', '|---|---|'] + [f'| {k} | {cell(v)} |' for k, v in rows]
    sets = [(f'`{k}={v}`', 'pause menu default on first launch: ' + MENU.get(k, k)) for k, v in p.settings.items()]
    if p.pacing != 'adaptive': sets.append((f'`pacing {p.pacing}`', 'frame pacing preset: ' + (', '.join(android_profile.PACING[p.pacing]) or 'none')))
    for v in p.values('env'):
        k = v.split('=', 1)[0]
        sets.append((f'`{v}`', short(reg[k].doc if k in reg else '') or '-'))
    if sets:
        out += ['', 'The profile sets these for you (`android.tsv`); a player changes the pause menu ones in the game, and the env rows in the data pack\'s `env.txt`:', '',
                '| Row | What it does |', '|---|---|'] + [f'| {cell(a)} | {cell(b)} |' for a, b in sets]
    out += ['', f'Read the profile\'s porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list]({TO_REPO}README.md#games).']
    return '\n'.join(out)


def game_page(name, g, p):
    """(path, old text, new text): a page without the made block gets it on top; a game with no page starts from the template"""
    path = g.dir / 'README.md'
    old = path.read_text(encoding='utf-8') if path.is_file() else ''
    base = old
    if FACTS_BEGIN not in base or FACTS_END not in base:
        base = TEMPLATE.read_text(encoding='utf-8').split(FACTS_END, 1)[1] if not base.strip() else '\n\n' + base
        base = FACTS_BEGIN + '\n' + FACTS_END + base
    return path, old, replace(base, FACTS_BEGIN, FACTS_END, facts(name, g, p), str(path.relative_to(REPO)))


def missing(text):   # what a page still lacks: required sections, and the template's TODO lines left in it
    out = [f'no section {h!r}' for h in REQUIRED if not re.search('^' + re.escape(h) + r'\s*$', text, flags=re.M)]
    return out + (['TODO lines from the template'] if re.search(r'\bTODO:', text) else [])


def checklist(text):
    games = REPO / 'nxnative/dm/games'
    def mark(m):
        return f"- [{'x' if (games / m.group(2) / 'android.tsv').is_file() else ' '}] {m.group(1)}"
    text = re.sub(r'^- \[[ x]\] (.*? · `(0100[0-9A-F]{12})`)', mark, text, flags=re.M)
    done = len(re.findall(r'^- \[x\] ', text, flags=re.M)); total = len(re.findall(r'^- \[[ x]\] ', text, flags=re.M))
    return re.sub(r'^\*\*\d+ / \d+ ported\*\*$', f'**{done} / {total} ported**', text, count=1, flags=re.M)


def main(argv):
    check = '--check' in argv; bad = []; todo = []
    jobs = [(REPO / 'README.md', None, readme), (REPO / 'docs/PORT_CHECKLIST.md', None, checklist)]
    for path, old, make in jobs:
        old = path.read_text(encoding='utf-8'); new = make(old)
        if new != old:
            if check: bad.append(str(path.relative_to(REPO)))
            else: path.write_text(new, encoding='utf-8', newline='\n'); print(f'wrote {path.relative_to(REPO)}')
    for name, g, p in listed():
        path, old, new = game_page(name, g, p)
        rel = str(path.relative_to(REPO)).replace('\\', '/')
        if new != old:
            if check: bad.append(rel)
            else: path.write_text(new, encoding='utf-8', newline='\n'); print(f'wrote {rel}')
        if missing(new): todo.append(f'{rel}: {", ".join(missing(new))} (docs/GAME_README_TEMPLATE.md)')
    if todo: print('game pages to finish by hand:\n  ' + '\n  '.join(todo))
    if bad: print('not what the profiles make: ' + ', '.join(bad) + ' (run python docs/gen_tables.py)')
    if bad or todo: return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
