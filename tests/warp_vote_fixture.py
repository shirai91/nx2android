"""Maxwell VOTE returns a 32-lane ballot and a separate predicate."""
import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compute_warp_fixture import GUARD, export_cases
from warp_shuffle_fixture import SEED, SHUFFLES, expected as shuffle_values
from resource_pool_fixture import base, mov, shader

PT = 7 << 16


def store_pair():
    # R8 and R9 form a 64-bit store to output[tid * 2]. R14 holds tid.
    return [base('Ldc') | (5 << 48) | (3 << 36) | (56 << 20) | PT | (255 << 8) | 12,
            mov(15, 3), base('Shl') | (15 << 20) | PT | (14 << 8) | 14,
            base('Iadd') | (14 << 20) | PT | (12 << 8) | 12,
            base('Stg') | (5 << 48) | (1 << 45) | PT | (12 << 8) | 8]


def vote(mode, split, inverse):
    ops = [base('S2r') | (33 << 20) | PT | 14, mov(10, split),
           base('Isetp') | (1 << 49) | (7 << 39) | (10 << 20) | PT | (14 << 8) | 7,
           base('Vote') | (mode << 48) | (1 << 45) | (int(inverse) << 42) | PT | 8,
           mov(10, 1), base('Sel') | (1 << 39) | (255 << 20) | PT | (10 << 8) | 9]
    return shader(ops + store_pair())


def expected(width, mode, split, inverse):
    words = []
    for start in range(0, width, 32):
        preds = [(tid < split) != inverse for tid in range(start, min(start + 32, width))]
        mask = sum(1 << lane for lane, pred in enumerate(preds) if pred)
        result = (all(preds), any(preds), len(set(preds)) == 1)[mode]
        words.extend([mask, int(result)] * len(preds))
    return words + [GUARD] * 8


def shuffle_predicate(width, step):
    mode, selector, control = step
    ops = [base('S2r') | (33 << 20) | PT | 14, mov(10, SEED),
           base('Iadd') | (10 << 20) | PT | (14 << 8) | 8,
           base('Shfl') | (control << 34) | (mode << 30) | (3 << 28) | (selector << 20) | PT | (8 << 8) | 8,
           mov(10, 1), base('Sel') | (255 << 20) | PT | (10 << 8) | 9]  # P0 -> R9
    words = []
    for tid, data in enumerate(shuffle_values(width, (step,))[:width]):
        lane = tid % 32
        first = lane & (control >> 8)
        last = first | (control & 31 & ~(control >> 8))
        indexed = first | (selector & ~(control >> 8))
        valid = (indexed <= last, lane - selector >= first, lane + selector <= last, (lane ^ selector) <= last)[mode]
        words.extend((data, int(valid)))
    return shader(ops + store_pair()), words + [GUARD] * 8


def generate(work):
    programs = []
    for width in (36, 64):
        for mode in range(3):
            for split in (0, 3, 31, 32, 33, width):
                for inverse in (False, True):
                    key = f'{0xfeed9e00 + len(programs):016x}'
                    programs.append((key, [width, 1, 1], vote(mode, split, inverse),
                                     expected(width, mode, split, inverse), {}))
    # Inspect the instruction's own validity bit independently of the selected register.
    for step in [*SHUFFLES[2], *SHUFFLES[3], (3, 2, 1), (1, 1, 0x1c00), (2, 1, 0x1c03), (0, 5, 3)]:
        binary, words = shuffle_predicate(64, step)
        key = f'{0xfeed9e00 + len(programs):016x}'
        programs.append((key, [64, 1, 1], binary, words, {}))
    return export_cases(work, programs)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    print(generate(parser.parse_args().directory.resolve()))
