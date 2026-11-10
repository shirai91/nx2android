# Physical controller profiles (dm/android/controllers.tsv): how a controller's keys reach the game. porter port --controller <id | file> writes the chosen one into the
# data pack as controller.tsv, which the app reads at launch (front_android.c pad_load). A profile states what the face buttons say (labels) and which Android key each
# sends (keys), so the pause menu's Face buttons row (Position / Label) means the same on every controller; more keys can go to any Switch button.
#
# A player's own file (--controller my.tsv), one <key> <TAB> <value> row each:
#   base    <id>                  start from a built-in profile (default: standard)
#   name    <text>
#   labels  xbox | nintendo | playstation
#   keys    position | label
#   map     <key> <TAB> <target>  an Android key name (BUTTON_C, BUTTON_L2, BUTTON_1 …) or code to a Switch button (A B X Y L R ZL ZR PLUS MINUS LS RS UP DOWN LEFT RIGHT),
#                                 a face button by its place (south east west north), or none
from dataclasses import dataclass, field
from pathlib import Path

from porter.paths import DM


class ControllerError(ValueError): pass   # a bad profile: the port turns it into a BuildError before any stage

TABLE = DM / 'android/controllers.tsv'
PACK_FILE = 'controller.tsv'
DEFAULT = 'standard'
KINDS = ('generic', 'handheld', 'controller')
LABELS = ('xbox', 'nintendo', 'playstation')
KEYS = ('position', 'label')
FACE = ('south', 'east', 'west', 'north')   # front_android.c's face places 1..4
BUTTONS = ('A', 'B', 'X', 'Y', 'L', 'R', 'ZL', 'ZR', 'PLUS', 'MINUS', 'LS', 'RS', 'UP', 'DOWN', 'LEFT', 'RIGHT')
TARGETS = FACE + BUTTONS + ('none',)
# Android's key codes (android/keycodes.h) a controller sends; any other code can be given as a number
KEYCODES = {'DPAD_UP': 19, 'DPAD_DOWN': 20, 'DPAD_LEFT': 21, 'DPAD_RIGHT': 22, 'DPAD_CENTER': 23, 'BUTTON_A': 96, 'BUTTON_B': 97, 'BUTTON_C': 98, 'BUTTON_X': 99,
            'BUTTON_Y': 100, 'BUTTON_Z': 101, 'BUTTON_L1': 102, 'BUTTON_R1': 103, 'BUTTON_L2': 104, 'BUTTON_R2': 105, 'BUTTON_THUMBL': 106, 'BUTTON_THUMBR': 107,
            'BUTTON_START': 108, 'BUTTON_SELECT': 109, **{f'BUTTON_{n}': 187 + n for n in range(1, 17)}}
MENU_KEYS = {4: 'BACK', 82: 'MENU', 110: 'BUTTON_MODE'}   # the pause menu's (front_android.c handle_event): never remapped, so a controller always reaches the menu
KEY_MAX = 511   # front_android.c PAD_KEYS - 1
NAMES = {v: k for k, v in KEYCODES.items()}
# where each face key's button is: by position, Android's standard; by label, where the printed letter is (Nintendo's A is on the right)
FACE_KEYS = ('BUTTON_A', 'BUTTON_B', 'BUTTON_X', 'BUTTON_Y')
PLACES = {('position', l): ('south', 'east', 'west', 'north') for l in LABELS}
PLACES[('label', 'xbox')] = PLACES[('position', 'xbox')]
PLACES[('label', 'nintendo')] = ('east', 'south', 'north', 'west')


@dataclass
class Controller:
    id: str
    name: str
    kind: str = 'generic'
    labels: str = 'xbox'
    keys: str = 'position'
    maps: dict = field(default_factory=dict)   # {key code: target}, after the face keys

    def as_dict(self): return dict(id=self.id, name=self.name, kind=self.kind, labels=self.labels, keys=self.keys, maps={NAMES.get(k, str(k)): t for k, t in self.maps.items()})

    def rows(self):
        """[(key code, target, key name)]: the face keys by the profile's facts, then its own maps (which win)"""
        out = {KEYCODES[n]: p for n, p in zip(FACE_KEYS, PLACES[(self.keys, self.labels)])}
        out.update(self.maps)
        return [(k, t, NAMES.get(k, '')) for k, t in out.items()]

    def pack_text(self, spec):
        """controller.tsv for the data pack: what front_android.c pad_load reads"""
        rows = [f'# Written by nx2android\'s porter: the physical controller profile (porter port --controller {spec}); <TAB>-separated, read by the app at launch',
                '# key <TAB> <Android key code> <TAB> <south | east | west | north: a face button by its place, which the pause menu\'s Face buttons row resolves; a Switch button; none> <TAB> <the key\'s name>',
                f'name\t{self.name}', f'labels\t{self.labels}']
        rows += [f'key\t{k}\t{t}\t{n}'.rstrip('\t') for k, t, n in self.rows()]
        return '\n'.join(rows) + '\n'


def keycode(s, where):
    s = s.strip()
    n = s.upper().removeprefix('KEYCODE_')
    k = int(s) if s.isdigit() else KEYCODES.get(n, next((c for c, m in MENU_KEYS.items() if m == n), None))
    if k is None: raise ControllerError(f'{where}: unknown key {s!r}: an Android key name ({", ".join(list(KEYCODES)[:8])} …) or its code')
    if k in MENU_KEYS: raise ControllerError(f'{where}: {MENU_KEYS[k]} opens the pause menu and is not remapped')
    if not 0 < k <= KEY_MAX: raise ControllerError(f'{where}: key code {k} out of range (1-{KEY_MAX})')
    return k


def target(s, where):
    t = s.strip(); t = t.lower() if t.lower() in FACE + ('none',) else t.upper()
    if t not in TARGETS: raise ControllerError(f'{where}: unknown target {s!r}: one of {" ".join(TARGETS)}')
    return t


def check(c, where):
    if c.kind not in KINDS: raise ControllerError(f'{where}: kind {c.kind!r}: one of {", ".join(KINDS)}')
    if c.labels not in LABELS: raise ControllerError(f'{where}: labels {c.labels!r}: one of {", ".join(LABELS)}')
    if c.keys not in KEYS: raise ControllerError(f'{where}: keys {c.keys!r}: one of {", ".join(KEYS)}')
    if (c.keys, c.labels) not in PLACES: raise ControllerError(f'{where}: keys by label needs letters on the buttons (xbox or nintendo labels), not {c.labels}')
    return c


def builtin():
    """{id: Controller} from dm/android/controllers.tsv, in the table's order"""
    out = {}
    for n, line in enumerate(TABLE.read_text(encoding='utf-8').splitlines(), 1):
        if not line.strip() or line.startswith('#'): continue
        where = f'{TABLE.name} line {n}'
        cols = line.split('\t')
        if len(cols) < 5: raise ControllerError(f'{where}: <id> <kind> <labels> <keys> <name> [<key>=<target> …]')
        c = Controller(cols[0], cols[4], cols[1], cols[2], cols[3])
        for m in cols[5:]:
            k, eq, t = m.partition('=')
            if not eq: raise ControllerError(f'{where}: {m!r} is not <key>=<target>')
            c.maps[keycode(k, where)] = target(t, where)
        if c.id in out: raise ControllerError(f'{where}: {c.id} twice')
        out[c.id] = check(c, where)
    return out


def from_file(path):
    """A player's own profile (the rows at the top of this file)"""
    p = Path(path).expanduser()
    try: text = p.read_text(encoding='utf-8')
    except OSError as e: raise ControllerError(f'--controller: cannot read {p}: {e}') from e
    rows = [(n, line.split('\t')) for n, line in enumerate(text.splitlines(), 1) if line.strip() and not line.startswith('#')]
    table = builtin(); base = next((cols[1].strip() for _, cols in rows if cols[0] == 'base' and len(cols) > 1), DEFAULT)
    if base not in table: raise ControllerError(f'{p.name}: base {base!r} is not a built-in controller profile (porter controllers)')
    b = table[base]; c = Controller(p.stem, p.stem, 'generic', b.labels, b.keys, dict(b.maps))
    for n, cols in rows:
        where = f'{p.name} line {n}'; k, v = cols[0].strip(), [x.strip() for x in cols[1:]]
        if k == 'base': continue
        if k in ('name', 'labels', 'keys') and v and v[0]: setattr(c, k, v[0])
        elif k == 'map' and len(v) >= 2: c.maps[keycode(v[0], where)] = target(v[1], where)
        else: raise ControllerError(f'{where}: expected base, name, labels, keys or map <key> <target> (tab-separated)')
    return check(c, p.name)


def resolve(spec=None):
    """(Controller, spec): a built-in id, or a .tsv file of the player's; empty: the standard one"""
    spec = (spec or '').strip() or DEFAULT
    table = builtin()
    if spec in table: return table[spec], spec
    if spec.lower().endswith('.tsv') or Path(spec).expanduser().is_file(): return from_file(spec), str(Path(spec).expanduser().resolve())
    raise ControllerError(f'--controller: no controller profile {spec!r}: one of {", ".join(table)} (porter controllers), or a .tsv file of your own')
