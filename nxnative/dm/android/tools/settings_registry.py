#!/usr/bin/env python3
# The settings registry (nxnative/dm/settings.tsv; docs/ARCHITECTURE.md, Settings): every DM_* switch the core reads, with its type, default, scope and doc.
# android_profile.py checks a profile's env rows with it, porter's data pack checks every env.txt row it writes, tests/test_settings.py checks the core against it.
#   python settings_registry.py check KEY=VALUE...   (scope profile)        python settings_registry.py show <name>
import re, sys
from pathlib import Path

PATH = Path(__file__).resolve().parents[2] / 'settings.tsv'
TYPES = ('bool', 'int', 'float', 'hex', 'str')
SCOPES = ('profile', 'porter', 'app', 'debug')


class Setting:
    __slots__ = ('name', 'type', 'default', 'scope', 'ios', 'when', 'where', 'doc')

    def __init__(self, row):
        c = row.split('\t')
        if len(c) != 7: raise ValueError(f'settings.tsv: {c[0]!r} has {len(c)} columns, not 7')
        self.name, self.type, self.default, scope, self.when, self.where, self.doc = c
        self.scope, _, ios = scope.partition(',')
        self.ios = ios == 'ios'
        if not re.fullmatch(r'DM_[A-Z0-9_]+', self.name): raise ValueError(f'settings.tsv: bad name {self.name!r}')
        if self.type not in TYPES: raise ValueError(f'settings.tsv: {self.name}: type {self.type!r} is not one of {", ".join(TYPES)}')
        if self.scope not in SCOPES or ios not in ('', 'ios'): raise ValueError(f'settings.tsv: {self.name}: scope {scope!r} is not one of {", ".join(SCOPES)} (with ",ios")')
        if self.when not in ('start', 'live', '?'): raise ValueError(f'settings.tsv: {self.name}: when {self.when!r} is not start, live or ?')
        if self.default and value_error(self, self.default): raise ValueError(f'settings.tsv: {self.name}: default {self.default!r}: {value_error(self, self.default)}')


def value_error(s, v):
    """Why v is not a value of setting s, or ''"""
    if s.type == 'bool' and v not in ('0', '1'): return 'want 0 or 1'
    if s.type == 'int' and not re.fullmatch(r'-?\d+', v): return 'want an integer'
    if s.type == 'float' and not re.fullmatch(r'-?(\d+\.?\d*|\.\d+)', v): return 'want a number'
    if s.type == 'hex' and not re.fullmatch(r'(0x)?[0-9A-Fa-f]+([,: ](0x)?[0-9A-Fa-f]+)*', v): return 'want hex'
    return ''


_cache = None
def load(path=PATH):
    """{name: Setting}"""
    global _cache
    if path == PATH and _cache is not None: return _cache
    out = {}
    for row in Path(path).read_text(encoding='utf-8').splitlines():
        if not row or row.startswith('#'): continue
        s = Setting(row)
        if s.name in out: raise ValueError(f'settings.tsv: {s.name} has two rows')
        out[s.name] = s
    if path == PATH: _cache = out
    return out


def check_env(rows, scopes, where=''):
    """Problems with KEY=VALUE rows set by someone allowed these scopes (['…'], empty when all is well). Non-DM keys are not the registry's"""
    reg = load(); bad = []
    for r in rows:
        k, eq, v = r.partition('=')
        if not eq or not k.startswith('DM_'): continue
        s = reg.get(k)
        if not s: bad.append(f'{where}{k}: no such setting (nxnative/dm/settings.tsv has every switch the core reads)'); continue
        if s.scope not in scopes: bad.append(f'{where}{k}: a {s.scope} setting; {" or ".join(scopes)} may not set it (change its scope in settings.tsv first)')
        if (e := value_error(s, v)): bad.append(f'{where}{k}={v}: {e} ({s.type})')
    return bad


if __name__ == '__main__':
    if sys.argv[1:2] == ['check']:
        bad = check_env(sys.argv[2:], ('profile',)); print('\n'.join(bad) or 'ok'); sys.exit(1 if bad else 0)
    if sys.argv[1:2] == ['show']:
        for n in sys.argv[2:]:
            s = load().get(n); print(f'{n}: ' + (f'{s.type}, default {s.default or "(code)"}, {s.scope}{",ios" if s.ios else ""}, {s.when}; {s.where}; {s.doc}' if s else 'not in the registry'))
        sys.exit(0)
    print(__doc__ or 'settings_registry.py check KEY=VALUE... | show <name>...'); sys.exit(2)
