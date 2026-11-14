"""Prove that every compute invocation reaches the same added warp barriers."""
from collections import defaultdict
from graphlib import CycleError, TopologicalSorter

from spv_module import opcode


def _uniform_trace(body, sensitive_functions, collective_ops):
    """For acyclic control flow, compare the last exchange at every join and return."""
    blocks = {}
    for inst in body:
        if opcode(inst) == 248:
            block = []
            blocks[inst[1]] = block
        elif blocks:
            block.append(inst)
    if not blocks:
        return False

    preds = {label: set() for label in blocks}
    exits = set()
    sites = set()
    for label, instructions in blocks.items():
        if not instructions:
            return False
        end = instructions[-1]
        op = opcode(end)
        if op == 249:  # Branch
            successors = end[1:2]
        elif op == 250:  # BranchConditional, possibly with weights
            successors = end[2:4]
        elif op in (253, 254):  # Return, ReturnValue
            successors = ()
            exits.add(label)
        else:
            return False
        for successor in successors:
            preds[successor].add(label)
        if any(opcode(i) in collective_ops or (opcode(i) == 57 and i[3] in sensitive_functions) for i in instructions):
            sites.add(label)

    # A label contains a fixed sequence of exchanges and function calls. If
    # incoming histories agree, recording the label uniquely identifies the
    # new history. This avoids materializing all paths through nested branches.
    last = {}
    entry = next(iter(blocks))
    try:
        for label in TopologicalSorter(preds).static_order():
            incoming = {last[parent] for parent in preds[label]} if preds[label] else {None}
            if len(incoming) != 1 or (label != entry and not preds[label]):
                return False
            last[label] = label if label in sites else incoming.pop()
    except CycleError:
        # A loop can run a different number of times per invocation.
        return False
    return len({last[label] for label in exits}) == 1


def converged(code, collective_ops):
    functions = {}
    callers = defaultdict(set)
    sensitive = set()
    body = None
    for inst in code:
        op = opcode(inst)
        if op == 54:
            function = inst[2]
            body = []
            functions[function] = body
        elif op == 56:
            body = None
        elif body is not None:
            body.append(inst)
            if op in collective_ops:
                sensitive.add(function)
            elif op == 57:
                callers[inst[3]].add(function)
    # An unconditional exchange inside a helper is still conditional when
    # a caller can skip that helper. Propagate the obligation to every caller.
    pending = list(sensitive)
    while pending:
        parents = callers[pending.pop()] - sensitive
        sensitive.update(parents)
        pending.extend(parents)
    return all(_uniform_trace(functions[fn], sensitive, collective_ops) for fn in sensitive)
