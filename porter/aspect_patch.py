import importlib.util, os, re, struct, zipfile
from pathlib import Path

from porter import fpslocker
from porter.paths import DM
from porter.upstream import BuildError, say


def prepare(source, main, title_id, out, extras):
    with main.open('rb') as f: header = f.read(0x100)
    build = header[0x40:0x54].hex()
    key = build[:32]
    if source.suffix.lower() == '.zip':
        ids = re.findall(r'\[([0-9a-fA-F]{16})\]', source.name)
        if ids and any(x.lower() != title_id.lower() for x in ids): raise BuildError('aspect patch ZIP title ID does not match the game')
        try:
            with zipfile.ZipFile(source) as z:
                entries = z.infolist()
                if len(entries) > 10000: raise BuildError('aspect patch ZIP has too many entries')
                candidates = [e for e in entries if not e.is_dir() and e.filename.lower().endswith('.pchtxt') and
                              any(re.fullmatch(r'\[?21[.:]9 ultrawide(?:[ \t][^\]/]*)?\]?', part, re.I) for part in e.filename.split('/')[:-1])]
                if len(candidates) > 64 or sum(e.file_size for e in candidates) > 8 * 1024 * 1024: raise BuildError('aspect patch ZIP has too many or too large candidate entries')
                if any(e.file_size > 1024 * 1024 for e in candidates): raise BuildError('aspect patch entry is too large')
                texts = [(e.filename, z.read(e).decode('utf-8-sig')) for e in candidates]
        except (OSError, zipfile.BadZipFile, UnicodeError) as e: raise BuildError(f'cannot read aspect patch ZIP: {e}') from e
    else:
        if source.stat().st_size > 1024 * 1024: raise BuildError('aspect patch file is too large')
        try: texts = [(source.name, source.read_text(encoding='utf-8-sig'))]
        except (OSError, UnicodeError) as e: raise BuildError(f'cannot read aspect patch: {e}') from e
    matches = [(name, text) for name, text in texts if re.search(r'(?im)^\s*@nsobid-' + build + r'\s*$', text)]
    if len(matches) != 1: raise BuildError(f'aspect patch: expected one match for full main build ID {build}, found {len(matches)}')
    name, text = matches[0]
    enabled = False
    stopped = False
    seen_id = False
    writes = []
    for line in text.splitlines():
        line = line.split('//', 1)[0].strip()
        if not line or line.startswith('#'): continue
        if line.lower() == '@stop': stopped = True; break
        if re.fullmatch(r'@nsobid-[0-9a-fA-F]{40}', line):
            if seen_id or line[8:].lower() != build: raise BuildError(f'aspect patch {name}: invalid build ID')
            seen_id = True
        elif line.lower() in ('@flag print_values', '@flag offset_shift 0x100'): pass
        elif line.lower() == '@enabled': enabled = True
        elif enabled and (m := re.fullmatch(r'([0-9a-fA-F]{8})\s+([0-9a-fA-F]+)', line)):
            if len(m[2]) % 2: raise BuildError(f'aspect patch {name}: invalid hex bytes')
            writes.append((int(m[1], 16), bytes.fromhex(m[2])))
        else: raise BuildError(f'aspect patch {name}: unsupported instruction {line!r}')
    if not seen_id or not stopped or not writes: raise BuildError(f'aspect patch {name}: incomplete patch')
    try: image, _ = fpslocker.nso_image(main)
    except fpslocker.Refused as e: raise BuildError(str(e)) from e
    segments = [struct.unpack_from('<III', header, 0x10 + i * 0x10)[1:] for i in range(3)]
    data_end = sum(segments[2])
    bss_end = data_end + struct.unpack_from('<I', header, 0x3c)[0]
    # Match patch_nso.py: FAR output does not persist BSS bytes.
    near = ((((bss_end + 0x3fff) & ~0x3fff) + 0x4000 + 0xffff) & ~0xffff) <= 0x7000000
    occupied = []
    for n, path in enumerate([DM / 'tools/sdk_patches.py', *sorted((DM / 'games').glob('*/patches.py')), *map(Path, filter(None, extras.split(os.pathsep)))]):
        spec = importlib.util.spec_from_file_location(f'aspect_existing_{n}', path); mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
        occupied += [(off, off + len(bytes.fromhex(old))) for off, old, _, _ in getattr(mod, 'CUTS', {}).get(key, [])]
        occupied += [(off, off + 4) for off, _, _, _ in getattr(mod, 'OBS', {}).get(key, [])]
    cuts = []
    for off, new in writes:
        end = off + len(new)
        initialized = any(start <= off and end <= start + size for start, size in segments)
        bss = data_end <= off and end <= bss_end
        if not initialized and not bss: raise BuildError(f'aspect patch {name}: {off:#x} is outside initialized NSO segments and BSS')
        if bss and not near: raise BuildError(f'aspect patch {name}: rewriter does not persist BSS patches for this module')
        if any(off < b and a < end for a, b in occupied): raise BuildError(f'aspect patch {name}: overlapping patch at {off:#x}')
        occupied.append((off, end))
        cuts.append((off, (bytes(len(new)) if bss else image[off:end]).hex(), new.hex(), f'ultrawide {name}'))
    dest = out / 'aspect_patches.py'
    dest.write_text(f'# Generated from the user-provided aspect patch\nCUTS = {{{key!r}: {cuts!r}}}\n', encoding='utf-8')
    say(f'ultrawide: {name}, main build {build}, {len(cuts)} cut(s); presentation 43:18')
    return dest
