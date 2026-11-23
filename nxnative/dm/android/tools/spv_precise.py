"""Match the Apple chain's precise min, max and saturation for NaN inputs."""
import math
import struct

from spv_builder import Module, instruction
from spv_module import decode, opcode, string_operand


def numeric_constants(module):
    """IDs whose components are all ordinary numbers or infinity."""
    numbers = set()
    for inst in module.code:
        op = opcode(inst)
        if op == 46 or (op == 44 and all(value in numbers for value in inst[3:])):
            numbers.add(inst[2])
        elif op == 43:
            shape = module.types[inst[1]]
            if opcode(shape) == 22:
                width = shape[2]
                bits = struct.pack(f'<{len(inst) - 3}I', *inst[3:])
                fmt = {16: '<e', 32: '<f', 64: '<d'}[width]
                if not math.isnan(struct.unpack_from(fmt, bits)[0]):
                    numbers.add(inst[2])
    return numbers


def emit_numeric_op(module, body, numbers, inst):
    ty, result, library, operation, a, b = inst[1:7]
    shape = module.types[ty]
    condition = module.ty(23, module.boolean, shape[3]) if opcode(shape) == 23 else module.boolean

    def or_numeric(value, fallback):
        if value in numbers:
            return value
        is_nan = module.value(body, 156, condition, value)
        return module.value(body, 169, ty, is_nan, fallback, value)

    def numeric(op, dst, left, right):
        # Native NMin/NMax can quiet a signaling NaN and return that NaN
        # on ARM64 Lavapipe. Supply the numeric operand on both sides first.
        lhs = or_numeric(left, right)
        rhs = or_numeric(right, left)
        body.append(instruction(12, ty, dst, library, op, lhs, rhs))

    if operation == 43 and b in numbers and inst[7] in numbers:
        # Saturation has constant bounds. One guard suffices for the clamp.
        body.append(instruction(12, ty, result, library, 81, or_numeric(a, b), b, inst[7]))
    elif operation == 43:  # clamp(x, lo, hi) = min(max(x, lo), hi)
        bounded = module.ident()
        numeric(80, bounded, a, b)
        numeric(79, result, bounded, inst[7])
    else:
        numeric(79 if operation == 37 else 80, result, a, b)


def lower(data):
    _, code = decode(data)
    glsl = {i[1] for i in code if opcode(i) == 11 and string_operand(i[2:]) == 'GLSL.std.450'}
    affected = {i[2] for i in code if opcode(i) == 12 and i[3] in glsl and i[4] in (37, 40, 43)}
    if not affected:
        return data

    module = Module(data)
    numbers = numeric_constants(module)
    body = []
    for inst in code:
        if opcode(inst) != 12 or inst[2] not in affected:
            body.append(inst)
            continue
        emit_numeric_op(module, body, numbers, inst)

    start = next(n for n, i in enumerate(body) if opcode(i) == 54)
    body[start:start] = module.declarations
    words = list(module.header)
    words[3] = module.next_id
    for inst in body:
        words.extend(inst)
    return struct.pack(f'<{len(words)}I', *words)
