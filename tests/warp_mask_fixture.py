"""Return Maxwell SR_LANEID and each SR_LANEMASK alongside a second observed value."""
import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compute_warp_fixture import GUARD, export_cases
from warp_vote_fixture import PT, store_pair
from resource_pool_fixture import base, mov, shader


def guest(sreg, mixed):
    ops = [base('S2r') | (33 << 20) | PT | 14, base('S2r') | (sreg << 20) | PT | 8,
           base('S2r') | PT | 9]  # SR_LANEID
    if mixed:
        ops.extend([mov(10, 0x9800), base('Iadd') | (10 << 20) | PT | (14 << 8) | 9,
                    base('Shfl') | (7 << 48) | (31 << 34) | (3 << 28) | PT | (9 << 8) | 9])
    return shader(ops + store_pair())


def generate(work):
    programs = []
    for width in (36, 64):
        for sreg in (0, 56, 57, 58, 59, 60):
            for mixed in (False, True):
                words = []
                for i in range(width):
                    lane = i % 32
                    eq = 1 << lane
                    mask = {0:lane, 56:eq, 57:eq-1, 58:eq*2-1, 59:~(eq*2-1), 60:~(eq-1)}[sreg]
                    words.extend((mask & 0xffffffff, (0x9800 + i - lane) if mixed else lane))
                key = f'{0xfeeda000 + len(programs):016x}'
                programs.append((key, [width,1,1], guest(sreg, mixed), words + [GUARD]*8, {}))
    return export_cases(work, programs)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    print(generate(parser.parse_args().directory.resolve()))
