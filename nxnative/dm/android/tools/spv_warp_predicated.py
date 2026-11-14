"""Move register-only warp exchanges to a common workgroup rendezvous."""
from collections import Counter
import struct
import subprocess

from spv_builder import Module, instruction
from spv_module import decode, opcode

ALL, ANY, EQUAL, BALLOT, SHUFFLE = 334, 335, 336, 339, 345
COLLECTIVES = {ALL, ANY, EQUAL, BALLOT, SHUFFLE}
REGISTER_OPS = {81, 83, 124, 128, 130, 132, 164, 165, 166, 167, 168, 169,
                *range(170, 180), *range(194, 201), *COLLECTIVES}


def _boolean(module, value):
    op = 41 if value else 42
    for inst in module.code + module.declarations:
        if opcode(inst) == op:
            return inst[2]
    key = module.ident()
    module.declarations.append(instruction(op, module.boolean, key))
    return key


def _lift_arm(module, instructions, condition, positive, out):
    # Each vote must still see only its original arm's lanes. Predicated
    # shuffles read the registers of participating donors; all other donors
    # were undefined. Their original branch or return still guards each use.
    boolean = module.boolean
    active = condition
    for inst in instructions:
        op = opcode(inst)
        if op not in (ALL, ANY, EQUAL, BALLOT):
            out.append(inst)
            continue
        if not positive and active == condition:
            active = module.value(out, 168, boolean, condition)
        test = inst[4]
        unanimous = None
        if op in (ALL, EQUAL):
            universal = module.value(out, 169, boolean, active, test, _boolean(module, True))
            if op == ALL:
                out.append(instruction(ALL, inst[1], inst[2], inst[3], universal))
                continue
            unanimous = module.value(out, ALL, boolean, inst[3], universal)
        existential = module.value(out, 169, boolean, active, test, _boolean(module, False))
        if unanimous is not None:
            some = module.value(out, ANY, boolean, inst[3], existential)
            none = module.value(out, 168, boolean, some)
            out.append(instruction(166, boolean, inst[2], unanimous, none))
        else:
            out.append(instruction(op, inst[1], inst[2], inst[3], existential))


def lift(data, optimizer):
    # Inline guest shuffle helpers and promote register temporaries first.
    flags = ['--preserve-bindings', '--inline-entry-points-exhaustive', '--eliminate-local-single-block',
             '--ssa-rewrite', '--eliminate-dead-code-aggressive', '--simplify-instructions',
             '--eliminate-dead-code-aggressive', '--cfg-cleanup']
    run = subprocess.run([optimizer, *flags, '-', '-o', '-'], input=data, capture_output=True)
    if run.returncode:
        return None
    header, code = decode(run.stdout)
    labels = {}
    incoming = Counter()
    selections = []
    for pos, inst in enumerate(code):
        op = opcode(inst)
        if op == 248:
            labels[inst[1]] = pos
        elif op == 249:
            incoming[inst[1]] += 1
        elif op == 250:
            incoming.update(inst[2:4])
            if opcode(code[pos - 1]) == 247:
                selections.append((pos - 1, code[pos - 1][1], inst))
        elif op == 251:  # uncounted switch edges could enter a candidate arm
            return None

    module = Module(run.stdout)
    moved = set()
    insertions = {}
    for header_pos, merge, branch in selections:
        arms = branch[2:4]
        for index, arm in enumerate(arms):
            if incoming[arm] != 1 or arms[0] == arms[1]:
                continue
            start = labels[arm] + 1
            end = start
            while opcode(code[end]) in REGISTER_OPS:
                end += 1
            if arm == merge:
                # An early return lets the other lanes skip all following
                # barriers. Hoist only the register-only prefix immediately
                # after that return, before either path has accessed memory.
                other = labels[arms[1 - index]] + 1
                if opcode(code[other]) != 253:
                    continue
            elif opcode(code[end]) != 249 or code[end][1] != merge:
                continue
            sites = [n for n in range(start, end) if opcode(code[n]) in COLLECTIVES]
            if not sites:
                continue
            end = sites[-1] + 1
            _lift_arm(module, code[start:end], branch[1], index == 0,
                      insertions.setdefault(header_pos, []))
            moved.update(range(start, end))
    if not moved:
        return None
    words = list(header)
    words[3] = module.next_id
    first = next(pos for pos, inst in enumerate(code) if opcode(inst) == 54)
    insertions[first] = module.declarations + insertions.get(first, [])
    for pos, inst in enumerate(code):
        for lifted in insertions.get(pos, ()):
            words.extend(lifted)
        if pos not in moved:
            words.extend(inst)
    return struct.pack(f'<{len(words)}I', *words)
