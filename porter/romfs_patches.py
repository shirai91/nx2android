import hashlib, json, mmap, struct

from porter.upstream import BuildError, trinity


def prepare(profile, version, romfs, work, decode):
    """Apply exact-hash Lua instruction reorder recipes; decode(job, result, bnsh=False) returns complete archive entries."""
    spec = profile / 'romfs_patches.json'
    if not spec.is_file(): return {}
    rules = json.loads(spec.read_text(encoding='utf-8')).get(version, [])
    out = {}
    for rule in rules:
        if rule.get('type') != 'lua54_reorder': raise BuildError(f"RomFS patch: unsupported recipe type {rule.get('type')!r}")
        path = rule['path']; loose = romfs / path
        work.mkdir(parents=True, exist_ok=True)
        if loose.is_file():
            data = loose.read_bytes()
        else:
            archive = romfs / rule['archive']
            packs = trinity.packs(str(archive))
            with archive.open('rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as d:
                entries = [e for off, _, _ in packs for e in trinity.pack_files(d, off) if e[1] == int(rule['file_hash'], 16)]
                if len(entries) != 1: raise BuildError(f'RomFS patch: expected one archive entry for {path}, found {len(entries)}')
                _, _, comp, size, start, length = entries[0]
                raw = bytes(d[start:start + length])
            if comp == 3:
                key = hashlib.sha256(raw).hexdigest()
                cache = work / (key + '.json')
                if cache.is_file():
                    record = json.loads(cache.read_text(encoding='utf-8'))
                    cached = work / record['output']
                    if (any(s['sha256'] == record['source'] and s['output'] == record['output'] for s in rule['sources'])
                            and cached.is_file() and hashlib.sha256(cached.read_bytes()).hexdigest() == record['output']):
                        out[path] = cached
                        continue
                job, result = work / 'job.bin', work / 'job.out'
                job.write_bytes(b'DMOJ' + struct.pack('<III', 1, length, size) + raw)
                decode(job, result, bnsh=False)
                answer = result.read_bytes()
                if len(answer) != size + 12 or answer[:8] != b'DMOR\x01\0\0\0' or struct.unpack_from('<I', answer, 8)[0] != size:
                    raise BuildError(f'RomFS patch: invalid decoder result for {path}')
                data = answer[12:]
            elif length == size: data = raw
            else: raise BuildError(f'RomFS patch: unsupported archive compression for {path}')
        digest = hashlib.sha256(data).hexdigest()
        source = next((s for s in rule['sources'] if digest in (s['sha256'], s['output'])), None)
        if source is None: raise BuildError(f'RomFS patch: unknown source SHA-256 for {path}: {digest}')
        if digest != source['output']:
            order = [i for start, end in rule['order'] for i in range(start, end)]
            positions = {old: new for new, old in enumerate(order)}
            positions[len(order)] = len(order)
            code = struct.unpack_from(f'<{len(order)}I', data, source['offset'])
            words = []
            for new, old in enumerate(order):
                word = code[old]
                if word & 127 == 56:
                    target = old + 1 + (word >> 7) - 16777215
                    word = 56 | ((positions[target] - new - 1 + 16777215) << 7)
                words.append(word)
            data = bytearray(data)
            struct.pack_into(f'<{len(order)}I', data, source['offset'], *words)
        if hashlib.sha256(data).hexdigest() != source['output']: raise BuildError(f'RomFS patch: output SHA-256 mismatch for {path}')
        target = work / source['output']
        target.write_bytes(data)
        if not loose.is_file() and comp == 3:
            cache.write_text(json.dumps({'source': source['sha256'], 'output': source['output']}), encoding='utf-8')
        out[path] = target
    return out
