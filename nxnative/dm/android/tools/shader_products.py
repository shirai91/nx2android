"""Bounded export checks and expected shader inventories; no graphics driver required."""
import json
import re
from pathlib import Path
import struct

import dmsv


MODELS = (0, 4, 3, 1, 2, 5)  # NVN stage -> SPIR-V execution model


def validate_spirv(data, stage):
    # Bounded decoding adapted from dm/android/tools/spv_module.py in
    # thehobbit85/nxnative at 1f01e28c87da (MIT, see nxnative/LICENSE).
    if stage not in range(6) or len(data) < 20 or len(data) % 4:
        raise ValueError('invalid shader stage or truncated SPIR-V')
    w = struct.unpack(f'<{len(data) // 4}I', data)
    if w[0] != 0x07230203 or w[1] not in (0x10000, 0x10100, 0x10200, 0x10300) or not w[3] or w[4]:
        raise ValueError('invalid SPIR-V header for Vulkan 1.1')
    at = 5; models = []
    while at < len(w):
        n, op = w[at] >> 16, w[at] & 0xffff
        if not n or at + n > len(w): raise ValueError(f'invalid SPIR-V instruction at word {at}')
        if op == 15:
            if n < 4: raise ValueError('truncated SPIR-V entry point')
            models.append(w[at + 1])
        at += n
    if models != [MODELS[stage]]: raise ValueError('SPIR-V entry point does not match shader stage')


def check_batch(result, count):
    # dmsh_xlate returns 1 for a completed batch containing unsupported shaders.
    # Its final summary distinguishes that result from a crash or truncated run.
    lines = result.stdout.rstrip().splitlines()
    summary = re.fullmatch(r'translated: (\d+) ok, (\d+) failed', lines[-1]) if lines else None
    if summary:
        ok, bad = map(int, summary.groups())
        if ok + bad == count and result.returncode == bool(bad):
            return
    raise RuntimeError('shader translator did not complete its batch: ' + result.stderr[-2000:])


def validate_resource(resource, key, stage):
    if not isinstance(resource, dict) or resource.get('key') != key or resource.get('stage') != ('vs','ps','gs','hs','ds','cs')[stage]:
        raise ValueError(f'{key}: translator reflection identity mismatch')
    fields = dict(cbufs=('binding','slot'), sbufs=('binding','slot','cb_slot','cb_offset','flags'),
                  textures=('binding','type','cbuf_slot','handle','array','flags'),
                  images=('binding','type','cbuf_slot','handle','array','flags'))
    for name, integers in fields.items():
        rows = resource.get(name)
        if not isinstance(rows, list): raise ValueError(f'{key}: missing reflection {name}')
        bindings = set()
        for row in rows:
            if not isinstance(row, dict) or any(type(row.get(f)) is not int for f in integers):
                raise ValueError(f'{key}: invalid reflection {name} row')
            if row['binding'] < 0 or row['binding'] in bindings: raise ValueError(f'{key}: invalid reflection binding')
            bindings.add(row['binding'])
            if name in ('textures','images') and (not isinstance(row.get('format'), str) or type(row.get('separate')) is not bool):
                raise ValueError(f'{key}: invalid image reflection')
    if 'fetch1d' in resource and (not isinstance(resource['fetch1d'], list) or any(type(x) is not int for x in resource['fetch1d'])):
        raise ValueError(f'{key}: invalid fetch1d reflection')


def validate_bindings(bindings, resource):
    # classify() uses the ordered SBuffer rows to recover guest addresses and writes.
    sbufs = sorted(resource['sbufs'], key=lambda x: x['binding'])
    for binding in bindings:
        match = re.fullmatch(r'[a-z]+_s(\d+)', binding['name'])
        if match:
            index = int(match.group(1))
            if index >= len(sbufs) or sbufs[index]['binding'] != binding['binding']:
                raise ValueError('storage buffer reflection does not match SPIR-V')


def output(directory, key, stage):
    spv, reflection = Path(directory) / f'{key}.spv', Path(directory) / f'{key}.res.json'
    if not spv.is_file(): return None
    data = spv.read_bytes(); validate_spirv(data, stage)
    if not reflection.is_file(): raise ValueError(f'{key}: translator omitted reflection')
    resource = json.loads(reflection.read_text(encoding='utf-8'))
    validate_resource(resource, key, stage)
    return data, resource


def check_bundle(path, report):
    """Validate the emitted inventory against the exporter's complete expected list."""
    if not isinstance(report, dict) or report.get('schema') != 1: raise ValueError('invalid shader export report')
    expected, failed = {}, set()
    for row in report.get('expected', []):
        if (not isinstance(row, list) or len(row) != 2 or not isinstance(row[0], str) or len(row[0]) != 16 or
                row[1] not in range(6)):
            raise ValueError('invalid expected shader')
        key = int(row[0], 16)
        if key in expected: raise ValueError('duplicate expected shader')
        expected[key] = row[1]
    for row in report.get('failed', []):
        if not isinstance(row, list) or len(row) != 2 or not isinstance(row[1], str): raise ValueError('invalid failed shader')
        key = int(row[0], 16)
        if key not in expected or key in failed: raise ValueError('unexpected failed shader')
        failed.add(key)
    emitted = {}
    with dmsv.Bundle(str(path)) as bundle:
        for e in bundle.entries:
            key, stage = e[:2]
            if key in emitted or expected.get(key) != stage or key in failed: raise ValueError('unexpected shader bundle entry')
            emitted[key] = stage
    if set(emitted) | failed != set(expected): raise ValueError('shader export inventory is incomplete')
    return len(emitted), len(expected), not failed
