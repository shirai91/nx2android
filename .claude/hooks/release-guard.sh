#!/bin/sh
# Runs release_guard.py (the PreToolUse hook in .claude/settings.json) with the first Python 3 found; without one the command goes ahead unchecked.
here=$(dirname "$0")
for py in "py -3" python3 python; do
    if $py -c 'import sys; sys.exit(sys.version_info < (3, 8))' >/dev/null 2>&1; then
        exec $py "$here/release_guard.py"
    fi
done
exit 0
