"""Runtime Vulkan descriptor pools with the shared Apple pool wrapper's bounds."""
import struct

from spv_builder import Module, instruction
from spv_module import opcode, string_operand, pointer_operands
MAX_BINDINGS = 96  # nx2android descriptor binding capacity

TABLES = {'tex_htab', 'samp_htab'}
COUNTS = {'tex_pool': 1 << 20, 'samp_pool': 1 << 12, 'tex_htab': 8192, 'samp_htab': 8192}


def fields(binding):
    kind = binding['kind']
    if kind == 'pool_bounds':
        count = binding.get('reg')
        if binding['type'] != 'ubo' or binding['count'] != 1 or type(count) is not int or not 1 <= count < MAX_BINDINGS:
            raise ValueError('invalid descriptor pool bounds block')
    if kind == 'htab_map' and (binding['type'] != 'ssbo' or binding['count'] != 1 or
                               type(binding.get('reg')) is not int or not 0 <= binding['reg'] < MAX_BINDINGS):
        raise ValueError('invalid uniform handle table map')
    if kind not in COUNTS: return ()
    types = {'tex_pool': ('tex',), 'samp_pool': ('samp',), 'tex_htab': ('tex', 'cis'), 'samp_htab': ('samp',)}
    slot = binding.get('pool_slot')
    if (binding['count'] != COUNTS[kind] or binding.get('runtime') is not True or
            type(slot) is not int or not 0 <= slot < MAX_BINDINGS - 1 or
            binding['type'] not in types[kind]):
        raise ValueError('invalid runtime descriptor pool')
    extra = ()
    if kind in TABLES:
        sampler = binding if kind == 'samp_htab' else binding.get('samp', {})
        sources = [binding]
        if binding['type'] == 'cis':
            if sampler.get('kind') != 'samp_htab': raise ValueError('missing combined handle-table sampler')
            sources.append(sampler)
        for source in sources:
            if (type(source.get('reg')) is not int or not 0 <= source['reg'] < 16 or
                    type(source.get('off')) is not int or not 0 <= source['off'] <= 65532 or source['off'] % 4):
                raise ValueError('invalid uniform handle table source')
        if binding['type'] == 'cis':
            extra = (f"soff={sampler['off']}",)
        if 'sreg' in sampler or 'soff' in sampler:
            offset = sampler.get('soff')
            if sampler.get('sreg') != 255 or type(offset) is not int or not 0 <= offset <= 4092 or offset % 4:
                raise ValueError('invalid uniform table driver sampler')
            extra += (f'c2_or={offset}',)
    return ('runtime=1', f'pool_slot={slot}', *extra)


def lower(data, reflection):
    resources = {b['msl_name']: b for b in reflection['bindings'] if b['kind'] in COUNTS}
    if not resources: return data, reflection
    m = Module(data)
    names = {x[1]: string_operand(x[2:]) for x in m.code if opcode(x) == 5}
    pools = {}; descriptors = []; folded_samplers = set()
    for x in m.code:
        if opcode(x) != 59 or 33 not in m.decorations[x[2]]: continue
        if m.decorations[x[2]].get(34) != (0,): raise ValueError('pool shader requires set-zero descriptors')
        descriptors.append(x[2])
        name = names.get(x[2]); resource = resources.get(name)
        if resource is None: continue
        pointer = m.types[x[1]]; array = m.types[pointer[3]]
        if (x[3] != 0 or opcode(array) != 28 or
                m.constants.get(array[3]) != COUNTS[resource['kind']]):
            raise ValueError('unexpected shared descriptor pool array')
        slot = len(pools); runtime = m.ty(29, array[2]); pointer = m.ty(32, 0, runtime)
        table = resource['kind'] in TABLES
        if table and opcode(m.types[array[2]]) == 27:
            folded_samplers.add(name + 'Smplr')
        pools[x[2]] = dict(slot=slot, name=name, table=table)
        m.declarations.append(instruction(59, pointer, x[2], 0))
    table_count = sum(pool['table'] for pool in pools.values())
    has_bounds = table_count != len(pools)
    if (set(resources) != {p['name'] for p in pools.values()} | folded_samplers or not pools or
            len(descriptors) + table_count + has_bounds > MAX_BINDINGS):
        raise ValueError('incomplete or oversized descriptor pool interface')
    # SPIRV-Cross numbers a binding after a whole million-element array. The
    # sidecar follows these compact bindings; guest sources remain unchanged.
    descriptors.sort(key=lambda var: m.decorations[var][33])
    compact = {var: index for index, var in enumerate(descriptors)}
    helpers = []
    if has_bounds:
        array = m.aggregate(28, m.uint, m.const(len(pools))); m.decorate(array, 6, 16)
        block = m.aggregate(30, array); m.decorate(block, 2)
        m.annotations.append(instruction(72, block, 0, 35, 0))
        bounds = m.variable(block, 2, 'dm_pool_bounds'); m.decorate(bounds, 34, 0); m.decorate(bounds, 33, len(descriptors))
        helpers.append(dict(msl_name='dm_pool_bounds', kind='pool_bounds', reg=len(pools)))
    if table_count:
        # Each map starts with its logical length and ends with a dummy slot.
        # Sparse handles high in a uniform buffer consume one descriptor per
        # distinct live resource, irrespective of the highest logical index.
        array = m.aggregate(29, m.uint); m.decorate(array, 6, 4)
        block = m.aggregate(30, array); m.decorate(block, 2)
        m.annotations += [instruction(72, block, 0, 35, 0), instruction(72, block, 0, 24)]
        map_pointer = m.ty(32, 12, m.uint)
        for pool in pools.values():
            if not pool['table']: continue
            name = 'dm_htab_map_' + str(pool['slot'])
            var = m.variable(block, 12, name); m.decorate(var, 34, 0)
            m.decorate(var, 33, len(descriptors) + len(helpers))
            helpers.append(dict(msl_name=name, kind='htab_map', reg=pool['slot']))
            pool['map'] = var
    word_pointer = m.ty(32, 2, m.uint) if has_bounds else None
    zero = m.const(0)
    rewritten = []; nonuniform = set(); seen = set()
    for x in m.code:
        op = opcode(x)
        if op == 59 and x[2] in pools: continue
        if op == 71 and x[1] in compact and x[2] == 33:
            x = instruction(71, x[1], 33, compact[x[1]])
        if op in (65, 66) and x[3] in pools:
            if len(x) != 5: raise ValueError('nested descriptor pool index')
            seen.add(x[3]); slot = pools[x[3]]['slot']
            index = m.value(rewritten, 124, m.uint, x[4])
            if pools[x[3]]['table']:
                table = pools[x[3]]['map']
                pointer = m.value(rewritten, 65, map_pointer, table, zero, zero)
            else:
                pointer = m.value(rewritten, 65, word_pointer, bounds, zero, m.const(slot))
            limit = m.value(rewritten, 61, m.uint, pointer)
            smaller = m.value(rewritten, 176, m.boolean, index, limit)
            index = m.value(rewritten, 169, m.uint, smaller, index, limit)
            if pools[x[3]]['table']:
                element = m.value(rewritten, 128, m.uint, index, m.const(1))
                pointer = m.value(rewritten, 65, map_pointer, table, zero, element)
                index = m.value(rewritten, 61, m.uint, pointer)
            x = instruction(op, x[1], x[2], x[3], index); nonuniform.update((index, x[2]))
        elif any(var in pools for var in pointer_operands(x)):
            # The shared pool_arrays wrapper likewise accepts indexed uses
            # only. Refuse escaped whole-array values, not arbitrary layouts.
            raise ValueError('unsupported whole descriptor pool use')
        if ((op == 61 and x[3] in nonuniform) or
                (op in (83, 100) and x[3] in nonuniform) or
                (op == 86 and any(value in nonuniform for value in x[3:]))):
            nonuniform.add(x[2])
        rewritten.append(x)
    if seen != set(pools): raise ValueError('unused descriptor pool cannot be bounded')
    for value in sorted(nonuniform):
        if 5300 not in m.decorations[value]: m.decorate(value, 5300)
    caps = {x[1] for x in rewritten if opcode(x) == 17}
    extensions = {string_operand(x[1:]) for x in rewritten if opcode(x) == 10}
    prefix = [x for x in rewritten if opcode(x) == 17]
    prefix += [instruction(17, cap) for cap in (5301, 5302, 5307) if cap not in caps]
    prefix += [x for x in rewritten if opcode(x) == 10]
    if 'SPV_EXT_descriptor_indexing' not in extensions:
        name = b'SPV_EXT_descriptor_indexing\0'; name += b'\0' * (-len(name) % 4)
        prefix.append(instruction(10, *struct.unpack(f'<{len(name)//4}I', name)))
    rewritten = prefix + [x for x in rewritten if opcode(x) not in (17, 10)]
    annotation_at = next((j for j, x in enumerate(rewritten) if opcode(x) in (71, 72, 73, 74, 75)),
                         next(j for j, x in enumerate(rewritten) if 19 <= opcode(x) <= 39))
    type_at = next(j for j, x in enumerate(rewritten) if 19 <= opcode(x) <= 39)
    function_at = next(j for j, x in enumerate(rewritten) if opcode(x) == 54)
    words = list(m.header); words[3] = m.next_id
    for j, x in enumerate(rewritten):
        if j == annotation_at:
            for value in m.names: words.extend(value)
        if j == type_at:
            for value in m.annotations: words.extend(value)
        if j == function_at:
            for value in m.declarations: words.extend(value)
        words.extend(x)
    slots = {pool['name']: pool['slot'] for pool in pools.values()}
    bindings = [dict(b, runtime=True, pool_count=COUNTS[b['kind']], pool_slot=slots[b['msl_name']])
                if b['msl_name'] in slots else b for b in reflection['bindings']]
    bindings.extend(helpers)
    return struct.pack(f'<{len(words)}I', *words), dict(reflection, bindings=bindings)
