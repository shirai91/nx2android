"""Translate Maxwell SHFL register exchanges and write independent per-lane expectations."""
import argparse
import math
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compute_warp_fixture import GUARD, export_cases
from resource_pool_fixture import base, mov, shader

# (SHFL mode, immediate source or distance, segment mask and clamp)
SHUFFLES = (
    ((0, 0, 31),), ((0, 31, 31),), ((1, 1, 0),), ((2, 1, 31),),
    ((3, 16, 31),), ((3, 1, 31),), ((0, 2, 0x1c03),),
    ((3, 16, 31), (2, 1, 31), (3, 2, 31)),  # reuses scratch repeatedly
)
SEED = 0x123400


def guest_code(steps, local):
    pred = 7 << 16  # unconditional
    ops = [base('S2r') | (33 << 20) | pred | 14]  # R14 = thread ID x
    for dimension in (1, 2):
        if local[dimension] > 1:
            stride = math.prod(local[:dimension])
            assert stride & (stride - 1) == 0
            ops.extend([base('S2r') | ((33 + dimension) << 20) | pred | 16,
                        mov(10, stride.bit_length() - 1),
                        base('Shl') | (10 << 20) | pred | (16 << 8) | 16,
                        base('Iadd') | (16 << 20) | pred | (14 << 8) | 14])
    ops.extend([mov(10, SEED), base('Iadd') | (10 << 20) | pred | (14 << 8) | 8])
    for mode, selector, clamp in steps:
        ops.append(base('Shfl') | (7 << 48) | (clamp << 34) | (mode << 30) |
                   (3 << 28) | (selector << 20) | pred | (8 << 8) | 8)
    # Store the final R8 in the guest output buffer at thread ID x * 4.
    ops.extend([base('Ldc') | (5 << 48) | (3 << 36) | (56 << 20) | pred | (255 << 8) | 12,
                mov(15, 2), base('Shl') | (15 << 20) | pred | (14 << 8) | 14,
                base('Iadd') | (14 << 20) | pred | (12 << 8) | 12,
                base('Stg') | (4 << 48) | (1 << 45) | pred | (12 << 8) | 8])
    return shader(ops)


def expected(width, steps):
    values = [SEED + tid for tid in range(width)]
    for mode, selector, control in steps:
        result = []
        for tid in range(width):
            lane = tid % 32
            segment = (control >> 8) & 31
            minimum = lane & segment
            maximum = minimum | (control & 31 & ~segment)
            source = (minimum | (selector & ~segment), lane - selector,
                      lane + selector, lane ^ selector)[mode]
            valid = source >= minimum if mode == 1 else source <= maximum
            result.append(values[tid - lane + source] if valid else values[tid])
        values = result
    return values + [GUARD] * 8


def generate(work):
    programs = []
    for local in ([32, 1, 1], [64, 1, 1], [128, 1, 1], [8, 4, 2], [36, 1, 1]):
        width = math.prod(local)
        # The last partial warp has four active lanes. Compare only shuffles
        # whose selected source exists; inactive sources have undefined data.
        variants = SHUFFLES if width % 32 == 0 else [SHUFFLES[i] for i in (0, 2, 5, 6)]
        for steps in variants:
            key = f'{0xfeed9d00 + len(programs):016x}'
            programs.append((key, local, guest_code(steps, local), expected(width, steps), {}))
    return export_cases(work, programs)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    print(generate(parser.parse_args().directory.resolve()))
