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


def file_state(product):
    """Cheap identity for a file product; directories always use their inventories."""
    product = Path(product)
    if not product.is_file(): return None
    st = product.stat()
    # Windows ctime is creation time, so the fast path cannot detect a
    # same-size edit that restores the original mtime.
    if os.name == 'nt': return [st.st_size, st.st_mtime_ns]
    return [st.st_size, st.st_mtime_ns, st.st_ctime_ns, st.st_dev, st.st_ino]


def reused(path, key, product, companions=()):
    try:
        record = json.loads(Path(path).read_text(encoding='utf-8'))
        if (not isinstance(record, dict) or record.get('schema') != 2 or record.get('key') != key or
                type(record.get('complete')) is not bool or not isinstance(record.get('note'), str)):
            return None
        if record.get('companions', []) != [inventory(p) for p in companions]: return None
        state = file_state(product)
        if state is not None and record.get('stat') == state: return record['note']
        if record.get('files') != inventory(product): return None
        if state is not None:
            if file_state(product) != state: return None
            record['stat'] = state
            write_json(path, record)
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


def stamp(path, key, product, note='', complete=True, companions=()):
    state = file_state(product)
    files = inventory(product)
    if file_state(product) != state: raise ValueError('product changed while recording its receipt')
    write_json(path, dict(schema=2, key=key, note=note, complete=complete, files=files, stat=state,
                          companions=[inventory(p) for p in companions]))
