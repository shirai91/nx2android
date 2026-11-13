"""Read Maxwell's five lane comparison masks from the logical compute lane."""
from spv_builder import instruction
from spv_module import opcode, pointer_operands


MASKS = {4416, 4417, 4418, 4419, 4420}  # Eq, Ge, Gt, Le, Lt


def inputs(module, builtins):
    """Recognize whole mask loads and constant component loads. Reject escaped pointers."""
    pointers: dict[int, tuple[int, int | None]] = {
        variable: (kind, None) for kind, variable in builtins.items() if kind in MASKS
    }
    if not pointers:
        return pointers
    for inst in module.code:
        op = opcode(inst)
        if op in (65, 66) and inst[3] in pointers:
            kind, parent = pointers[inst[3]]
            component = module.constants.get(inst[4]) if len(inst) == 5 else None
            if parent is not None or component not in (0, 1, 2, 3):
                return None
            pointers[inst[2]] = kind, component
        elif op != 61 and any(operand in pointers for operand in pointer_operands(inst)):
            return None
    return pointers


def emit(module, code, inst, index, kind, component):
    zero = module.const(0)
    if component in (1, 2, 3):
        code.append(instruction(83 if inst[1] == module.uint else 124, inst[1], inst[2], zero))
        return
    linear = module.value(code, 61, module.uint, index)
    lane = module.value(code, 199, module.uint, linear, module.const(31))
    bit = module.value(code, 196, module.uint, module.const(1), lane)
    mask = bit
    if kind != 4416:
        lower = module.value(code, 130, module.uint, bit, module.const(1))
        mask = module.value(code, 197, module.uint, lower, bit) if kind in (4418, 4419) else lower
        if kind in (4417, 4418):
            mask = module.value(code, 200, module.uint, mask)
    if component is None:
        unsigned = module.ty(23, module.uint, 4)
        if inst[1] == unsigned:
            code.append(instruction(80, unsigned, inst[2], mask, zero, zero, zero))
        else:
            vector = module.value(code, 80, unsigned, mask, zero, zero, zero)
            code.append(instruction(124, inst[1], inst[2], vector))
    else:
        code.append(instruction(83 if inst[1] == module.uint else 124, inst[1], inst[2], mask))
