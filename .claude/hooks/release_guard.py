# PreToolUse hook (.claude/settings.json): checks release commands against CLAUDE.md's release rules before they run.
#   gh release create|upload|edit  needs --repo shirai91/nx2android (gh here defaults to seedds/nxnative) and attaches no zip (the
#                                  release-windows.yml and release-macos.yml workflows build and attach them on publish, --clobber)
#   gh release create <tag>        and git tag <tag>: the tag is v + porter/__init__.py's __version__ (bump it first)
# A refused command gets the reason back; an allowed gh release create gets the steps that follow publishing.
import json, re, shlex, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REPO = 'shirai91/nx2android'


def version():
    m = re.search(r"^__version__ = '([^']+)'", (ROOT / 'porter/__init__.py').read_text(encoding='utf-8'), re.M)
    return m and m.group(1)


def words(cmd):
    try: return shlex.split(cmd, posix=True)
    except ValueError: return cmd.split()


def segments(cmd):
    """The simple commands of a shell line (split on ; && || | and newlines), as word lists."""
    return [words(s) for s in re.split(r'&&|\|\||[;|\n]', cmd) if s.strip()]


def check_gh(w):
    sub = w[2] if len(w) > 2 else ''
    if sub not in ('create', 'upload', 'edit'): return None
    repo = next((w[i + 1] for i, a in enumerate(w[:-1]) if a in ('--repo', '-R')), None) or next((a.split('=', 1)[1] for a in w if a.startswith('--repo=')), None)
    if repo != REPO:
        return f'gh release {sub} needs --repo {REPO}: gh in this checkout defaults to seedds/nxnative, which is not this project.'
    if sub in ('create', 'upload') and any(a.lower().endswith('.zip') or '.zip#' in a.lower() for a in w):
        return ('Do not attach the bundle zips by hand: on publish, .github/workflows/release-windows.yml and release-macos.yml build them on '
                "GitHub's runners and attach them with --clobber (CLAUDE.md, Releases). Create the release without a zip.")
    if sub == 'create':
        tag = next((a for a in w[3:] if not a.startswith('-')), None)
        v = version()
        if tag and v and tag != f'v{v}':
            return f'The tag {tag} is not v{v}, the version in porter/__init__.py. Bump it first in a commit "chore(release): version <x.y.z>".'
    return None


def check_git_tag(w):
    tags = [a for a in w[2:] if re.fullmatch(r'v\d+\.\d+\.\d+', a)]
    if not tags or any(a in ('-d', '--delete', '-l', '--list', '-v', '--verify') for a in w): return None
    v = version()
    if v and tags[0] != f'v{v}':
        return f'The tag {tags[0]} is not v{v}, the version in porter/__init__.py. Bump it first in a commit "chore(release): version <x.y.z>".'
    return None


AFTER = ('Release rules (CLAUDE.md, Releases): after publishing, watch the "Windows release" and "macOS release" runs '
         f'(gh run list --repo {REPO} --limit 4) and check that both zips are on the release; a failed run means the release has no zip for that OS.')


def main():
    data = json.load(sys.stdin)
    cmd = (data.get('tool_input') or {}).get('command') or ''
    if 'release' not in cmd and 'tag' not in cmd: return
    creates = False
    for w in segments(cmd):
        while w and re.fullmatch(r'\w+=.*', w[0]): w = w[1:]   # VAR=value prefixes
        reason = None
        if len(w) > 1 and Path(w[0]).stem == 'gh' and w[1] == 'release':
            reason = check_gh(w); creates |= reason is None and len(w) > 2 and w[2] == 'create'
        elif len(w) > 1 and Path(w[0]).stem == 'git' and w[1] == 'tag':
            reason = check_git_tag(w)
        if reason:
            print(json.dumps({'hookSpecificOutput': {'hookEventName': 'PreToolUse', 'permissionDecision': 'deny', 'permissionDecisionReason': reason}}))
            return
    if creates:
        print(json.dumps({'hookSpecificOutput': {'hookEventName': 'PreToolUse', 'additionalContext': AFTER}}))


if __name__ == '__main__':
    main()
