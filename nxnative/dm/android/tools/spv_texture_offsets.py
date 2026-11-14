"""Exact signed-four-bit sample offsets for Vulkan 1.1.

Use the hardware's constant-offset sample so mip selection, filtering and
sampler clamps remain intact. Explicit LOD/gradient samples branch inside a
helper function, leaving the caller's loop/selection structure unchanged.
Implicit samples evaluate every candidate in the original derivative context;
selecting a result must not introduce lane-varying derivative control flow.
"""
import itertools
import struct

from spv_builder import Module, instruction
from spv_module import decode, opcode
from spv_module import pointer_operands


def lower(data):
    """Lower only proven Maxwell-style offsets; reject unknown ranges."""
    _, code = decode(data)
    samples = [(x, 6 if opcode(x) in (89, 90, 93, 94) else 5) for x in code if 87 <= opcode(x) <= 94]
    samples = [(x, mask) for x, mask in samples if len(x) > mask and x[mask] & 0x10]
    if not samples:
        return data
    m = Module(data)
    # Typed result instructions start with ResultType, ResultId. OpUndef and
    # OpExtInst precede the type/constant section in the opcode enumeration;
    # decorations can name a type but do not define a typed result.
    definitions = {x[2]: x for x in code if len(x) >= 3 and x[1] in m.types and
                   (opcode(x) in (1, 12) or opcode(x) >= 41) and opcode(x) not in (71, 72, 74, 75)}
    stores = {}
    for x in code:
        if opcode(x) == 62: stores.setdefault(x[1], []).append(x[2])
    replacements = {}; functions = []

    def resolve(value, active=()):
        if value in active: raise ValueError('cyclic texture offset definition')
        x = definitions.get(value, ())
        if not x: raise ValueError('unknown texture offset definition')
        op = opcode(x)
        if op in (83, 124): return resolve(x[3], (*active, value))
        if op == 61:
            variable = definitions.get(x[3], ())
            assigned = stores.get(x[3], [])
            if (len(variable) == 4 and opcode(variable) == 59 and variable[3] == 7 and len(assigned) == 1):
                # Only direct local loads/stores may use this pointer. A passed
                # or aliased pointer could have a writer outside the store map.
                for use in code:
                    if use is variable or opcode(use) in (5, 71): continue
                    if opcode(use) == 61 and use[3] == x[3]: continue
                    if opcode(use) == 62 and use[1] == x[3]: continue
                    op = opcode(use)
                    refs = use[3:4] if op in (68, 117, 227, 318) or 229 <= op <= 242 else (
                        use[1:2] if op in (228, 319) else pointer_operands(use))
                    if x[3] in refs: raise ValueError('escaped texture offset local')
                return resolve(assigned[0], (*active, value))
        return x

    def component(value):
        x = resolve(value); ty = m.types[x[1]]
        if opcode(ty) != 21 or ty[2] != 32: raise ValueError('texture offset is not a 32-bit integer')
        if opcode(x) == 43:
            v = x[3] - (1 << 32) if x[3] & (1 << 31) else x[3]
            if not -8 <= v <= 7: raise ValueError('texture offset exceeds portable signed-four-bit range')
            return (v,)
        if opcode(x) in (202, 203):
            width = m.constants.get(x[5])
            if width is not None and 1 <= width <= (4 if opcode(x) == 202 else 3):
                return tuple(range(-(1 << (width-1)), 1 << (width-1))) if opcode(x) == 202 else tuple(range(1 << width))
        raise ValueError('unproven texture sample offset range')

    for sample, mask_at in samples:
        mask = sample[mask_at]
        # ImageOperands before Offset: Bias, Lod, Grad(two IDs), ConstOffset.
        offset_at = mask_at + 1 + bool(mask & 1) + bool(mask & 2) + 2*bool(mask & 4) + bool(mask & 8)
        if offset_at >= len(sample) or mask & 0x28: raise ValueError('invalid mixed texture sample offsets')
        offset_id = sample[offset_at]; offset = resolve(offset_id); offset_type = m.types[definitions[offset_id][1]]
        if opcode(offset_type) == 23:
            if opcode(offset) not in (44, 80) or len(offset[3:]) != offset_type[3]:
                raise ValueError('texture offset vector is not constructed from bounded components')
            components = list(offset[3:]); scalar = offset_type[2]
        else:
            components = [offset_id]; scalar = definitions[offset_id][1]
        if len(components) > 3: raise ValueError('unsupported texture offset dimension')
        domains = [component(v) for v in components]
        tuples = list(itertools.product(*domains))
        # Parameters preserve all original operands (including the typed image
        # and coordinate); only the dynamic offset becomes a literal per case.
        arg_positions = [i for i in range(3, len(sample)) if i not in (mask_at, offset_at)]
        args = [sample[i] for i in arg_positions]
        sampled_image = definitions[args[0]]
        while opcode(sampled_image) == 83:
            sampled_image = definitions[sampled_image[3]]
        separate = opcode(sampled_image) == 86
        # OpSampledImage's result may not be passed to a function. Pass the
        # separate image and sampler, then combine them in the sampling block.
        if separate: args = [*sampled_image[3:5], *args[1:]]
        arg_types = [definitions[value][1] for value in args]
        arg_types.append(m.uint)
        result_type = sample[1]; fn = m.ident(); function_type = m.ty(33, result_type, *arg_types)
        body = [instruction(54, result_type, fn, 0, function_type)]
        parameters = [m.value(body, 55, ty) for ty in arg_types]
        body.append(instruction(248, m.ident()))
        # Nonuniform resource selection remains nonuniform across the helper.
        nonuniform = any(5300 in values for values in m.decorations.values())
        if nonuniform:
            for parameter in parameters[:2 if separate else 1]: m.decorate(parameter, 5300)
        caller = []; selector = m.const(0)
        for dim in range(len(components)):
            value = m.value(caller, 81, scalar, offset_id, dim) if len(components) > 1 else offset_id
            value = m.value(caller, 124, m.uint, value) if scalar != m.uint else value
            value = m.value(caller, 199, m.uint, value, m.const(15))
            if dim: value = m.value(caller, 196, m.uint, value, m.const(dim*4))
            selector = m.value(caller, 197, m.uint, selector, value)
        index = parameters[-1]
        keys = [sum((v & 15) << (dim*4) for dim, v in enumerate(values)) for values in tuples]
        explicit = opcode(sample) in (88, 90, 92, 94)
        labels = [m.ident() for _ in tuples] if explicit else []
        merge = m.ident() if explicit else None
        if explicit:
            body += [instruction(247, merge, 0), instruction(251, index, labels[0], *[v for pair in zip(keys[1:], labels[1:]) for v in pair])]
        results = []; selected = None
        for ordinal, values in enumerate(tuples):
            if explicit: body.append(instruction(248, labels[ordinal]))
            constants = [m.const(v & 0xffffffff, scalar) for v in values]
            if len(constants) == 1:
                const_offset = constants[0]
            else:
                const_offset = m.ident(); m.declarations.append(instruction(44, definitions[offset_id][1], const_offset, *constants))
            operands_parameters = parameters[:-1]
            if separate:
                # Keep this combination beside its sample; OpSampledImage's
                # result cannot cross the case's block boundary either.
                combined = m.value(body, 86, sampled_image[1], *parameters[:2])
                if nonuniform: m.decorate(combined, 5300)
                operands_parameters = [combined, *parameters[2:-1]]
            args_by_pos = dict(zip(arg_positions, operands_parameters)); args_by_pos[offset_at] = const_offset
            operands = [mask & ~0x10 | 8 if i == mask_at else args_by_pos[i] for i in range(3, len(sample))]
            result = m.value(body, opcode(sample), result_type, *operands)
            if explicit:
                results += [result, labels[ordinal]]; body.append(instruction(249, merge))
            elif selected is None:
                selected = result
            else:
                condition = m.value(body, 170, m.boolean, index, m.const(keys[ordinal]))
                result_info = m.types[result_type]
                if opcode(result_info) == 23:
                    condition = m.value(body, 80, m.ty(23, m.boolean, result_info[3]), *([condition]*result_info[3]))
                selected = m.value(body, 169, result_type, condition, result, selected)
        if explicit:
            body.append(instruction(248, merge)); selected = m.value(body, 245, result_type, *results)
        body += [instruction(254, selected), instruction(56)]
        functions += body
        caller.append(instruction(57, result_type, sample[2], fn, *args, selector))
        replacements[sample[2]] = caller

    words = list(m.header); words[3] = m.next_id
    annotations_done = declarations_done = False
    for x in code:
        if not annotations_done and 19 <= opcode(x) <= 39:
            for a in m.annotations: words.extend(a)
            annotations_done = True
        if not declarations_done and opcode(x) == 54:
            for d in m.declarations: words.extend(d)
            declarations_done = True
        if 87 <= opcode(x) <= 94 and x[2] in replacements:
            for r in replacements[x[2]]: words.extend(r)
        else: words.extend(x)
    for x in functions: words.extend(x)
    return struct.pack(f'<{len(words)}I', *words)
