"""Keys of source code that change only when the code does: C# with its comments and whitespace left out. The porter builds the shader translator once for
every game and keys every game's shader bundle on it (porter/pipeline.py translator): a comment or a reformatted line in the translator's 357 sources must not
make every game translate its shaders again at its next port."""
import hashlib
from pathlib import Path


def cs_code(text):
    """C# source without its comments, with each line's runs of spaces and tabs as one space and blank lines dropped. String and character literals stay as
    they are (regular, verbatim @"...", interpolated $"..."). A file with a raw string literal (\"\"\") is returned as it is: its rules are not handled here"""
    if '"""' in text: return text
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and text.startswith('//', i):
            j = text.find('\n', i); i = n if j < 0 else j; continue
        if c == '/' and text.startswith('/*', i):
            j = text.find('*/', i + 2); i = n if j < 0 else j + 2; out.append(' '); continue
        if c == '"' or (c in '@$' and i + 1 < n and text[i + 1] in '"@$'):
            j = i
            verbatim = False
            while j < n and text[j] in '@$': verbatim |= text[j] == '@'; j += 1
            if j >= n or text[j] != '"': out.append(c); i += 1; continue
            j += 1
            while j < n:
                if verbatim:
                    if text[j] == '"':
                        if j + 1 < n and text[j + 1] == '"': j += 2; continue
                        j += 1; break
                else:
                    if text[j] == '\\': j += 2; continue
                    if text[j] == '"': j += 1; break
                    if text[j] == '\n': break   # an unterminated literal: the line ends it
                j += 1
            out.append(text[i:j]); i = j; continue
        if c == "'":
            j = i + 1
            while j < n and text[j] != "'" and text[j] != '\n': j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1]); i = j + 1; continue
        out.append(c); i += 1
    lines = []
    for line in ''.join(out).split('\n'):
        line = ' '.join(line.split())
        if line: lines.append(line)
    return '\n'.join(lines)


def file_key(path):
    """sha256 of a source file: its C# code (cs_code) for .cs, its bytes for anything else (a project file's text is its meaning)"""
    p = Path(path)
    data = cs_code(p.read_text(encoding='utf-8-sig', errors='surrogateescape')).encode('utf-8', 'surrogateescape') if p.suffix == '.cs' else p.read_bytes()
    return hashlib.sha256(data).hexdigest()
