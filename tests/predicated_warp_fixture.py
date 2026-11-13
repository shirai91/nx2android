"""Execute predicated guest shuffles with active source lanes across narrow subgroups."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compute_warp_fixture import GUARD, export_cases
from warp_shuffle_fixture import SEED
from warp_vote_fixture import PT, store_pair
from resource_pool_fixture import base, mov, shader


def less_than(predicate, a, b):
    return base('Isetp') | (1 << 49) | (7 << 39) | (b << 20) | PT | (a << 8) | (predicate << 3) | 7


def shuffle(split, inverse, donor):
    ops = [base('S2r') | (33 << 20) | PT | 14, mov(10, split), less_than(0, 14, 10),
           mov(8, SEED), base('Iadd') | (14 << 20) | PT | (8 << 8) | 8,
           base('Shfl') | (7 << 48) | (31 << 34) | (3 << 28) | (donor << 20) |
           (int(inverse) << 19) | (8 << 8) | 8, mov(9, 0x9abc)]
    words = []
    for tid in range(64):
        active = (tid < split) != inverse
        source = tid // 32 * 32 + donor
        assert not active or ((source < split) != inverse)
        words.extend((SEED + (source if active else tid), 0x9abc))
    return shader(ops + store_pair()), words


def generate(work):
    cases = [shuffle(split, inverse, 7 if split % 32 and not inverse else 31)
             for split in (32, 16, 48) for inverse in (False, True)]
    programs = [(f'{0xfeedb300 + i:016x}', [64, 1, 1], binary, words + [GUARD] * 8, {})
                for i, (binary, words) in enumerate(cases)]
    return export_cases(work, programs)


if __name__ == '__main__':
    generate(Path(sys.argv[1]).resolve())
