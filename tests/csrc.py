"""Cutting core C sources for the test harnesses (not a test module): one place for how a function or a region is found, and one error that says which
source changed. A harness compiles the cut with its own stubs, so a new symbol in a cut function needs a stub in that harness: the Tests workflow
(.github/workflows/tests.yml) compiles every harness on each push and pull request, before a merge.
"""
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


class CutError(AssertionError):
    pass


def read(rel):
    """A source of the repository (path relative to its root)"""
    return (REPO / rel).read_text(encoding='utf-8')


def _find(text, needle, start=0, what=''):
    i = text.find(needle, start)
    if i < 0: raise CutError(f'{what or "source"}: {needle!r} not found; the code moved or was renamed: update the cut and the stubs in this test')
    return i


def function(text, head, what=''):
    """The top-level function that starts at the first `head` (e.g. 'static void f(int x) {'), through its closing brace at column 0 and the newline"""
    start = _find(text, head, 0, what)
    return text[start:_find(text, '\n}\n', start, what) + 3]


def between(text, begin, end, what='', start=0):
    """From the first `begin` (at or after start) up to, not including, the next `end`"""
    s = _find(text, begin, start, what)
    return text[s:_find(text, end, s, what)]


def unit(path):
    """A core source with its section files put back: gfx_vk_exec.c includes gfx_vk_exec_<section>.inc files (one translation unit, one file a
    section). A harness that cuts from it reads it through this, so a function keeps its place in the cut whichever section file holds it"""
    import re
    p = Path(path)
    if not p.is_absolute(): p = REPO / p
    return re.sub(r'#include "(gfx_vk_exec_\w+\.inc)"[^\n]*\n', lambda m: (p.parent / m.group(1)).read_text(encoding='utf-8'), p.read_text(encoding='utf-8'))
