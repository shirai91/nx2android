"""Bounded export checks and expected shader inventories; no graphics driver required."""
import json
from pathlib import Path
import struct

import dmsv


MODELS = (0, 4, 3, 1, 2, 5)  # NVN stage -> SPIR-V execution model


def validate_spirv(data, stage):
    # Adapted from nxnative dm/android/tools/spv_module.py's bounded decoder.
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


def output(directory, key, stage):
    spv, reflection = Path(directory) / f'{key}.spv', Path(directory) / f'{key}.res.json'
    if not spv.is_file(): return None
    data = spv.read_bytes(); validate_spirv(data, stage)
    if not reflection.is_file(): raise ValueError(f'{key}: translator omitted reflection')
    resource = json.loads(reflection.read_text(encoding='utf-8'))
    if not isinstance(resource, dict): raise ValueError(f'{key}: invalid translator reflection')
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
            validate_spirv(bundle.spv(e), stage)
            r = bundle.refl(e)
            if len(r) < 20: raise ValueError('truncated shader reflection')
            bindings, constants = struct.unpack_from('<2I', r)
            if 20 + bindings * 20 + constants != len(r): raise ValueError('invalid shader reflection bounds')
            emitted[key] = stage
    if not emitted or set(emitted) | failed != set(expected): raise ValueError('shader export inventory is incomplete')
    return len(emitted), len(expected), not failed
