#!/usr/bin/env python3
# A game profile's Android keys: dm/games/<id>/android.tsv (key <TAB> value, # comments), beside the profile's game.tsv. A profile without one has not been ported to Android.
# Keys (docs/ARCHITECTURE.md "Game profiles on Android"):
#   status       playable | wip (plays, but parts are missing: docs/PORT_STATUS.md says which) | boots | experimental
#   tested       <version> <device>: what the status was checked on
#   package      the app's package name (default org.nx2a.t<title ID in lowercase>: a package name's parts start with a letter)
#   orientation  landscape (default) | any
#   requires     device features, separated by spaces: bcn (BC texture compression)
#   setting      <name>=<value>, one row each: the pause menu's defaults on first launch (overlay, rumble, docked, fps, opacity: 20..80 by 5)
#   env          <KEY>=<VALUE>, one row each: host switches the game needs on Android (the data pack's env.txt)
#   overlay      <tablet|phone> <control> <x> <y>: moves one control of that layout (dm/android/overlay_default.tsv) to x, y as fractions of the window
# Usage: android_profile.py header <id> <out.h> (android_profile.h for the host: the gamepad layouts, written only when it changes); android_profile.py list;
#   android_profile.py <id> [<key>] (the profile's keys, or one key's value); android_profile.py theme <id> (the gamepad theme pack porter puts in the APK)
# From Python: import android_profile; p = android_profile.load(<id>) (None without android.tsv); p.status, p.package, p.settings, p.env, p.overlay_table();
#   android_profile.theme(<title ID>): the theme pack's folder (dm/android/themes/themes.tsv, else the ergo pack)
import os, re, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ANDROID = HERE.parent
sys.path.insert(0, str(ANDROID.parent / 'games'))
import game   # noqa: E402  (the profile folders, DM_GAMES_DIR included)

STATUS = ('playable', 'wip', 'boots', 'experimental')
SETTINGS = {'overlay': ('0', '1'), 'rumble': ('0', '1'), 'docked': ('0', '1'), 'fps': ('0', '1'),   # the pause menu's settings (front_android.c settings_load)
            'opacity': tuple(str(v) for v in range(20, 81, 5))}                                    # the gamepad's, percent (overlay_layout.h OV_OPACITY_MIN..MAX)
REQUIRES = ('bcn',)
KINDS = {'stick_l': 'OV_STICK_L', 'stick_r': 'OV_STICK_R', 'button': 'OV_BUTTON', 'dpad': 'OV_DPAD', 'menu': 'OV_MENU'}
SINGLE = ('status', 'tested', 'package', 'orientation', 'requires')
REPEAT = ('setting', 'env', 'overlay')


class ProfileError(Exception):
    pass


LAYOUTS = ('tablet', 'phone')
THEMES = ANDROID / 'themes'


def default_layout():   # [{'layout', 'name', 'kind', 'anchor', 'x', 'y', 'w', 'h', 'button', 'label'}]: the tablet rows, then the phone rows, in drawing order
    rows = []
    for line in (ANDROID / 'overlay_default.tsv').read_text(encoding='utf-8').splitlines():
        if not line.strip() or line.startswith('#'): continue
        c = line.split('\t')
        if len(c) != 10 or c[0] not in LAYOUTS or c[2] not in KINDS: raise ProfileError(f'overlay_default.tsv: bad row {line!r}')
        rows.append(dict(zip(('layout', 'name', 'kind', 'anchor', 'x', 'y', 'w', 'h', 'button'), c[:9]), label='' if c[9] == '~' else c[9]))
    names = [[r['name'] for r in rows if r['layout'] == lay] for lay in LAYOUTS]
    if names[0] != names[1]: raise ProfileError('overlay_default.tsv: the layouts must have the same controls in the same order (a press is kept by its control\'s index)')
    return rows


def theme(gid):
    """The gamepad's theme pack for a title ID: its folder under dm/android/themes (themes.tsv), the ergo pack for a game with no row"""
    for line in (THEMES / 'themes.tsv').read_text(encoding='utf-8').splitlines():
        c = line.split('\t')
        if len(c) >= 2 and not line.startswith('#') and c[0].upper() == str(gid).upper():
            if not (THEMES / c[1] / 'theme.tsv').is_file(): raise ProfileError(f'themes.tsv: {gid} has the pack {c[1]!r}, which has no theme.tsv')
            return THEMES / c[1]
    return THEMES / 'ergo'


class AndroidProfile:
    def __init__(self, g):
        self.game, self.id, self.path = g, g.id, g.dir / 'android.tsv'
        self.rows = []
        for n, line in enumerate(self.path.read_text(encoding='utf-8').splitlines(), 1):
            if not line.strip() or line.lstrip().startswith('#'): continue
            k, _, v = line.partition('\t'); k, v = k.strip(), v.strip()
            if k not in SINGLE + REPEAT: raise ProfileError(f'{self.path}:{n}: unknown key {k!r} (keys: {", ".join(SINGLE + REPEAT)})')
            if k in SINGLE and any(r[0] == k for r in self.rows): raise ProfileError(f'{self.path}:{n}: {k} given twice')
            self.rows.append((k, v))
        self.check()

    def get(self, key, default=''):
        return next((v for k, v in self.rows if k == key), default)

    def values(self, key):
        return [v for k, v in self.rows if k == key]

    @property
    def name(self): return self.game.get('name', self.id)
    @property
    def status(self): return self.get('status', 'experimental')
    @property
    def tested(self): return self.get('tested')
    @property
    def package(self): return self.get('package') or f'org.nx2a.t{self.id.lower()}'
    @property
    def landscape(self): return self.get('orientation', 'landscape') == 'landscape'
    @property
    def requires(self): return self.get('requires').split()

    @property
    def settings(self):   # {'overlay': '1', ...}: the defaults the data pack's settings.txt starts with
        return dict(v.split('=', 1) for v in self.values('setting'))

    @property
    def env(self):   # ['KEY=VALUE', ...]
        return self.values('env')

    def overlay_table(self):
        """The gamepad layouts: the default ones with this profile's moves (a moved control's x is a fraction of the window's width: its anchor, no offset)"""
        rows = default_layout(); by = {(r['layout'], r['name']): r for r in rows}
        for v in self.values('overlay'):
            c = v.split()
            by[c[0], c[1]].update(anchor=c[2], x='0', y=c[3])
        return rows

    def check(self):
        bad = []
        if self.status not in STATUS: bad.append(f'status {self.status!r}: one of {", ".join(STATUS)}')
        if not re.fullmatch(r'[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+', self.package): bad.append(f'package {self.package!r} is not a package name')
        if self.get('orientation', 'landscape') not in ('landscape', 'any'): bad.append('orientation: landscape or any')
        bad += [f'requires {r!r}: one of {", ".join(REQUIRES)}' for r in self.requires if r not in REQUIRES]
        for v in self.values('setting'):
            k, eq, val = v.partition('=')
            if not eq or k not in SETTINGS or val not in SETTINGS[k]: bad.append(f'setting {v!r}: ' + ', '.join(f'{n}={"|".join(vs)}' for n, vs in SETTINGS.items()))
        bad += [f'env {v!r}: KEY=VALUE' for v in self.values('env') if not re.fullmatch(r'[A-Z_][A-Z0-9_]*=.*', v)]
        names = {r['name'] for r in default_layout()}
        for v in self.values('overlay'):
            c = v.split()
            if len(c) != 4 or c[0] not in LAYOUTS or c[1] not in names or not all(re.fullmatch(r'\d*\.?\d+', x) for x in c[2:]):
                bad.append(f'overlay {v!r}: <tablet|phone> <control> <x> <y>, control one of {", ".join(sorted(names))}')
        if bad: raise ProfileError(f'{self.path}: ' + '; '.join(bad))


def load(gid):
    """The Android profile of a game profile (a title ID or a folder), None when the game has no android.tsv"""
    g = game.load(gid)
    return AndroidProfile(g) if (g.dir / 'android.tsv').is_file() else None


def all_profiles():   # every game profile, with its Android profile or None
    return [(g, AndroidProfile(g) if (g.dir / 'android.tsv').is_file() else None) for g in game.all_games()]


def cfloat(s): return s + ('' if '.' in s else '.0') + 'f'


def header(p):
    """android_profile.h: the gamepad layouts as DM_AOV_TABLET and DM_AOV_PHONE (overlay_layout.h's OV_LAYOUTS), and what the profile says about itself"""
    rows = p.overlay_table() if p else default_layout()
    def table(lay):
        t = ',\n    '.join(f'{{{KINDS[r["kind"]]}, {cfloat(r["anchor"])}, {cfloat(r["x"])}, {cfloat(r["y"])}, {cfloat(r["w"])}, {cfloat(r["h"])}, '
                            f'{r["button"] if r["button"] != "-" else "0"}, "{r["label"]}", "{r["name"]}"}}' for r in rows if r['layout'] == lay)
        return t.replace(chr(10), ' ' + chr(92) + chr(10))
    src = f'dm/games/{p.id}/android.tsv and dm/android/overlay_default.tsv' if p else 'dm/android/overlay_default.tsv (the profile has no android.tsv)'
    return (f'// Generated by dm/android/tools/android_profile.py from {src}; do not edit\n#pragma once\n'
            f'#define DM_ANDROID_STATUS "{p.status if p else "none"}"\n#define DM_AOV_N {sum(r["layout"] == "tablet" for r in rows)}\n'
            f'#define DM_AOV_TABLET \\\n    {table("tablet")}\n#define DM_AOV_PHONE \\\n    {table("phone")}\n')


if __name__ == '__main__':
    a = sys.argv[1:]
    try:
        if len(a) == 3 and a[0] == 'header':
            out = Path(a[2]); g = game.load(a[1])
            text = header(AndroidProfile(g) if (g.dir / 'android.tsv').is_file() else None)
            out.parent.mkdir(parents=True, exist_ok=True)
            if not (out.exists() and out.read_text() == text):   # written only when it changes, as game.py's header: an unchanged header must not rebuild the host
                tmp = out.with_name(out.name + '.tmp'); tmp.write_text(text); tmp.replace(out)
        elif len(a) == 2 and a[0] == 'theme':
            print(theme(a[1]))
        elif a == ['list']:
            for g, p in all_profiles():
                if p: print(f'{g.id}\t{p.status}\t{p.package}\t{g.get("name", "")}')
        elif len(a) in (1, 2):
            p = load(a[0])
            if not p: sys.exit(f'{a[0]} has no android.tsv: not ported to Android')
            if len(a) == 2: print(getattr(p, a[1]) if a[1] in ('status', 'package', 'tested') else ' '.join(p.values(a[1])))
            else:
                for k, v in p.rows: print(f'{k}\t{v}')
        else:
            sys.exit('usage: android_profile.py header <id> <out.h> | list | theme <id> | <id> [<key>]')
    except ProfileError as e:
        sys.exit(f'[RED] {e}')
