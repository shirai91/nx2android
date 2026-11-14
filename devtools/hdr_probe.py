"""Inspect logged HDR dumps or per-draw pixel histories without device access.

  python devtools/hdr_probe.py dumps path/to/dump.log --output report.json
  python devtools/hdr_probe.py history path/to/pixel_history.tsv --output changes.json

Raw files must sit beside the log under their logged pass_NN.ppm.f32 names.
Only supported float formats in that log are read; leftover raw files belonging
to non-float dumps are ignored. Coordinates in a cropped dump are crop-relative.
"""
import argparse
from array import array
import csv
import json
import math
from pathlib import Path
import re
import sys

FLOAT_RGB = {76, 97, 122}  # R16_SFLOAT, R16G16B16A16_SFLOAT, B10G11R11_UFLOAT_PACK32.
META = re.compile(r'pass dump (\d+) is the submission.s pass (-?\d+) \(present (\d+)\)')
IMAGE = re.compile(r'pass dump (\d+): (\d+)x(\d+) format (\d+)\b')


def analyze_rgb(path, width, height, coordinate_limit=32):
    """Validate the RGB32F payload and report exact non-finites, not clipped PPMs."""
    path = Path(path)
    if width <= 0 or height <= 0 or coordinate_limit < 0:
        raise ValueError('Positive image dimensions and a non-negative coordinate limit required')
    if path.stat().st_size != width * height * 12:
        raise ValueError(f'{path}: expected {width * height * 12} RGB32F bytes')
    values = array('f')
    with path.open('rb') as stream:
        values.fromfile(stream, width * height * 3)
    if sys.byteorder != 'little':
        values.byteswap()
    nan = inf = finite = bad_pixels = 0
    minimum, maximum, total = math.inf, -math.inf, 0.0
    examples = []
    for pixel in range(width * height):
        bad = False
        rgb = values[3 * pixel:3 * pixel + 3]
        for value in rgb:
            if math.isnan(value):
                nan += 1
                bad = True
            elif math.isinf(value):
                inf += 1
                bad = True
            else:
                finite += 1
                minimum = min(minimum, value)
                maximum = max(maximum, value)
                total += value
        if bad:
            bad_pixels += 1
            if len(examples) < coordinate_limit:
                examples.append({'x': pixel % width, 'y': pixel // width,
                                 'rgb': [v if math.isfinite(v) else str(v) for v in rgb]})
    return {'width': width, 'height': height, 'nan_channels': nan, 'infinite_channels': inf,
            'nonfinite_pixels': bad_pixels, 'finite_channels': finite,
            'finite_min': minimum if finite else None, 'finite_max': maximum if finite else None,
            'finite_mean': total / finite if finite else None, 'examples': examples}


def analyze_dumps(log_path, coordinate_limit=32):
    log_path = Path(log_path)
    metadata, entries, seen = {}, [], set()
    for line in log_path.read_text(encoding='utf-8').splitlines():
        if match := META.search(line):
            index, render_pass, present = map(int, match.groups())
            metadata[index] = {'render_pass': render_pass, 'present': present}
        if match := IMAGE.search(line):
            index, width, height, fmt = map(int, match.groups())
            if index in seen:
                raise ValueError(f'{log_path}: dump index {index} reused; keep one capture batch per log/directory')
            seen.add(index)
            entry = {'dump': index, 'format': fmt, **metadata.get(index, {})}
            if fmt not in FLOAT_RGB:
                entry['status'] = 'skipped_non_hdr_format'
            else:
                raw = log_path.parent / f'pass_{index:02d}.ppm.f32'
                entry.update(analyze_rgb(raw, width, height, coordinate_limit), status='analyzed')
            entries.append(entry)
    if not entries:
        raise ValueError(f'{log_path}: no completed colour dump entries')
    return {'log': str(log_path), 'coordinates': 'relative to each captured image; add logged crop origin',
            'dumps': entries}


def analyze_history(path):
    with Path(path).open(encoding='utf-8', newline='') as stream:
        lines = [line for line in stream if not line.startswith('#')]
    reader = csv.DictReader(lines, delimiter='\t')
    required = {'event', 'frame', 'draw', 'kind', 'prepared', 'vs', 'fs', 'vb', 'ib',
                'count', 'instances', 'base', 'hdr_word', 'depth_word', 'depth_format'}
    if not required.issubset(reader.fieldnames or []):
        raise ValueError(f'{path}: missing pixel-history columns')
    rows = list(reader)
    changes, previous_color, previous_depth = [], None, None
    for row in rows:
        depth = row['depth_word'] if row['depth_format'] != '0' else None
        color_changed = row['hdr_word'] != previous_color
        depth_changed = depth != previous_depth
        if color_changed or depth_changed:
            changes.append({**row, 'color_changed': color_changed, 'depth_changed': depth_changed})
        previous_color, previous_depth = row['hdr_word'], depth
    matches = []
    for writer in changes:
        if not writer['depth_changed'] or writer['kind'] not in ('3', '4') or writer['prepared'] != '1':
            continue
        signature = tuple(writer[k] for k in ('frame', 'vb', 'ib', 'count', 'instances', 'base'))
        candidates = [row for row in rows if row['kind'] == writer['kind'] and
                      tuple(row[k] for k in ('frame', 'vb', 'ib', 'count', 'instances', 'base')) == signature]
        matches.append({'depth_writer': writer['event'], 'candidate_events': [r['event'] for r in candidates]})
    return {'events': len(rows), 'changes': changes, 'geometry_candidates': matches,
            'note': 'Logical draw identities and equal buffer/count fields are candidates, not proof of identical geometry or a GPU write.'}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('kind', choices=('dumps', 'history'))
    parser.add_argument('path', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--coordinate-limit', type=int, default=32)
    args = parser.parse_args(argv)
    try:
        result = analyze_dumps(args.path, args.coordinate_limit) if args.kind == 'dumps' else analyze_history(args.path)
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')
    text = json.dumps(result, indent=2, allow_nan=False)
    if args.output:
        args.output.write_text(text + '\n', encoding='utf-8')
    else:
        print(text)


if __name__ == '__main__':
    main()
