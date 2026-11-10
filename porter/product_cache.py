"""Content receipts for generated products, adapted from nxnative's Android export contracts."""
import hashlib
import json
import os
from pathlib import Path
import tempfile


def inventory(root):
    """Hash current files, never paths supplied by a receipt; include relative names and sizes."""
    root = Path(root)
    paths = [root] if root.is_file() else sorted(p for p in root.rglob('*') if p.is_file())
    if not paths: raise ValueError(f'empty or missing product: {root}')
    result = []
    for p in paths:
        h = hashlib.sha256(); size = 0
        with p.open('rb') as f:
            for block in iter(lambda: f.read(1024 * 1024), b''):
                h.update(block); size += len(block)
        result.append([p.name if p == root else p.relative_to(root).as_posix(), size, h.hexdigest()])
    return result


def reused(path, key, product):
    try:
        record = json.loads(Path(path).read_text(encoding='utf-8'))
        if (not isinstance(record, dict) or record.get('schema') != 1 or record.get('key') != key or
                record.get('complete') is not True or not isinstance(record.get('note'), str)):
            return None
        if record.get('files') != inventory(product): return None
        return record['note']
    except (OSError, ValueError, TypeError):
        return None


def write_json(path, record):
    path = Path(path)
    with tempfile.TemporaryDirectory(prefix='.receipt-', dir=path.parent) as scratch:
        tmp = Path(scratch) / 'receipt.json'
        with tmp.open('w', encoding='utf-8') as f:
            json.dump(record, f, sort_keys=True); f.write('\n'); f.flush(); os.fsync(f.fileno())
        os.replace(tmp, path)


def stamp(path, key, product, note='', complete=True):
    write_json(path, dict(schema=1, key=key, note=note, complete=complete, files=inventory(product)))
