import hashlib, json, re
from pathlib import Path

from porter.upstream import BuildError


def build(directory, patched, shaders, font_files, fps_files, package, title_id, version, profile_h):
    patched = Path(patched)
    if not profile_h: raise BuildError('runtime bundle needs the built host profile hash; port the game again')
    sources = {}
    modules = sorted(patched.glob('*.patched.bin'))
    if not modules: raise BuildError(f'{patched}: no rewritten modules for the runtime bundle')
    for binary in modules:
        name = binary.name[:-len('.patched.bin')]
        if not re.fullmatch(r'[A-Za-z0-9_-]+', name): raise BuildError(f'{binary}: invalid module name')
        for suffix in ('.patched.bin', '.fixups.tsv', '.gates.tsv'):
            source = patched / (name + suffix)
            if not source.is_file(): raise BuildError(f'{source}: missing runtime module file')
            sources['mods/' + source.name] = source
    nro = patched / 'nro.tsv'
    if nro.is_file():
        sources['mods/nro.tsv'] = nro
        for line in nro.read_text(encoding='utf-8').splitlines():
            if not line.strip() or line.startswith('#'): continue
            fields = line.split()
            if len(fields) != 3: raise BuildError(f'{nro}: invalid NRO row')
            if fields[0] != 'refused' and f'mods/{fields[0]}.patched.bin' not in sources:
                raise BuildError(f'{nro}: missing rewritten NRO {fields[0]}')
    if shaders: sources['shaders.dmsv'] = Path(shaders)
    sources.update(font_files)
    sources.update(fps_files)
    cleanup = {}
    for binary in modules:
        name = binary.name[:-len('.patched.bin')]
        for suffix in ('.cuts.tsv', '.obs.tsv', '.sites.tsv'):
            p = patched / (name + suffix)
            if p.is_file(): cleanup['mods/' + p.name] = p
    provenance = patched / 'build_src.sha256'
    if provenance.is_file(): cleanup['mods/build_src.sha256'] = provenance

    def records(files):
        result = []
        for name, source in sorted(files.items()):
            digest = hashlib.sha256()
            with source.open('rb') as f:
                for chunk in iter(lambda: f.read(1024 * 1024), b''): digest.update(chunk)
            result.append(dict(path=name, size=source.stat().st_size, sha256=digest.hexdigest()))
        return result

    manifest = dict(format=1, package=package, title_id=title_id, version=version, profile_h=profile_h,
                    files=records(sources), cleanup=records(cleanup))
    encoded = json.dumps(manifest, sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode('utf-8')
    directory = Path(directory); directory.mkdir(parents=True, exist_ok=True)
    path = directory / 'manifest.json'; path.write_bytes(encoded)
    flags = [a for name, source in sorted(sources.items()) for a in ('--asset', f'runtime/{name}={source}')]
    flags += ['--asset', f'runtime/manifest.json={path}']
    return manifest, hashlib.sha256(encoded).hexdigest(), flags
