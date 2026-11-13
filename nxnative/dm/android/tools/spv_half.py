"""Preserve Apple's round-to-nearest-even binary16 register packing on Vulkan.

GLSL PackHalf2x16 has implementation-defined rounding (Vulkan SPIR-V
environment, Precision of GLSL.std.450 Instructions). Integer conversion
avoids both that ambiguity and optional shaderFloat16/float-controls features.
Only the Android exporter applies this pass; the shared Apple chain is intact.
"""
import struct

from spv_builder import Module, instruction
from spv_module import decode, opcode, string_operand


def lower(data):
    _, code = decode(data)
    glsl = {x[1] for x in code if opcode(x) == 11 and string_operand(x[2:]) == 'GLSL.std.450'}
    packs = [x for x in code if opcode(x) == 12 and x[3] in glsl and x[4] == 58]
    if not packs:
        return data
    m = Module(data)
    fp32 = m.ty(22, 32); pair = m.ty(23, fp32, 2)
    fn = m.ident(); signature = m.ty(33, m.uint, pair)
    body = [instruction(54, m.uint, fn, 0, signature)]
    arg = m.value(body, 55, pair)
    body.append(instruction(248, m.ident()))

    def op(kind, *args):
        return m.value(body, kind, m.uint, *args)

    def less(a, b):
        return m.value(body, 176, m.boolean, a, m.const(b))

    def choose(condition, a, b):
        return op(169, condition, a, b)

    def half(component):
        value = m.value(body, 81, fp32, arg, component)
        bits = op(124, value)
        sign = op(199, op(194, bits, m.const(16)), m.const(0x8000))
        magnitude = op(199, bits, m.const(0x7fffffff))
        exponent = op(194, magnitude, m.const(23))
        mantissa = op(199, magnitude, m.const(0x7fffff))
        # Add half an output ULP minus one, then the retained low bit. This
        # rounds exact ties to even and propagates a mantissa carry naturally.
        odd = op(199, op(194, magnitude, m.const(13)), m.const(1))
        rounded = op(128, magnitude, op(128, m.const(0xfff), odd))
        normal = op(130, op(194, rounded, m.const(13)), m.const(112 << 10))
        # For half subnormals, discard 14..24 significand bits. Bound the shift
        # before evaluating either select arm so no oversized shift is emitted.
        small = less(magnitude, 0x38800000)  # 2^-14
        bounded = choose(less(exponent, 102), m.const(102), exponent)
        bounded = choose(small, bounded, m.const(112))
        shift = op(130, m.const(126), bounded)
        significand = op(197, mantissa, m.const(0x800000))
        odd = op(199, op(194, significand, shift), m.const(1))
        midpoint = op(196, m.const(1), op(130, shift, m.const(1)))
        bias = op(128, op(130, midpoint, m.const(1)), odd)
        subnormal = op(194, op(128, significand, bias), shift)
        subnormal = choose(less(magnitude, 0x33000000), m.const(0), subnormal)  # below 2^-25
        result = choose(small, subnormal, normal)
        # 65520 is the first finite value rounding to infinity. The Apple
        # conversion canonicalizes either NaN sign/payload to positive 0x7e00.
        result = choose(less(magnitude, 0x477ff000), result, m.const(0x7c00))
        is_nan = m.value(body, 172, m.boolean, magnitude, m.const(0x7f800000))
        return choose(is_nan, m.const(0x7e00), op(197, result, sign))

    lo = half(0); hi = half(1)
    packed = op(197, lo, op(196, hi, m.const(16)))
    body += [instruction(254, packed), instruction(56)]
    ids = {x[2] for x in packs}
    words = list(m.header); words[3] = m.next_id
    declared = False
    for x in code:
        if not declared and opcode(x) == 54:
            for declaration in m.declarations: words.extend(declaration)
            declared = True
        if opcode(x) == 12 and x[2] in ids:
            if x[1] != m.uint or len(x) != 6:
                raise ValueError('unsupported half-pack signature')
            words.extend(instruction(57, x[1], x[2], fn, x[5]))
        else:
            words.extend(x)
    for x in body: words.extend(x)
    return struct.pack(f'<{len(words)}I', *words)
