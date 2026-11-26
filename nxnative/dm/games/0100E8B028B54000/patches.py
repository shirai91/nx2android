# Trails in the Sky the 2nd (0100E8B028B54000): the patch of the instance culling compute shader.
# The patch works on the translated shader's SPIR-V words (dm/android/tools/spv_post.py, the binary path), so it needs no spirv-dis / spirv-as / spirv-val.
# The 1st Chapter's profile (PR #411) patches the same shader in its own build's ids; the ids here are this game's own (the translator numbered them
# differently), so every site is found by its structure, not by a number.
import spv_post

OP_STORE, OP_LOAD, OP_IEQUAL, OP_INOT_EQUAL, OP_SELECT, OP_VARIABLE, OP_CONSTANT, OP_CONSTFALSE, OP_TYPEPOINTER = 62, 61, 170, 171, 169, 59, 43, 42, 32
OP_FETCH, OP_EXTRACT, OP_FORD_GE, OP_FUNORD_GE, OP_BRANCH_COND, OP_LABEL, OP_RETURN, OP_FUNCTION, OP_LOGICAL_NOT = 95, 81, 190, 191, 250, 248, 253, 54, 168
OP_ACCESS_CHAIN, OP_SMOD, OP_UMOD, OP_TYPEARRAY, OP_TYPEINT = 65, 139, 137, 28, 21
OP_NAME, OP_MEMBERNAME, OP_ENTRYPOINT, OP_EXECMODE, OP_DECORATE, OP_DECORATEID, OP_DECORATESTR, OP_MEMBERDECORATESTR = 5, 6, 15, 16, 71, 78, 76, 77
STORAGE_FUNCTION, STORAGE_WORKGROUP, STORAGE_PRIVATE = 7, 4, 6
# opcodes that carry no result id (a value instruction's operands always are: result type, result id, ...): a scan of what computes a value skips these
NOT_VALUE = {OP_NAME, OP_MEMBERNAME, OP_ENTRYPOINT, OP_EXECMODE, OP_DECORATE, OP_DECORATEID, OP_DECORATESTR, OP_MEMBERDECORATESTR,
             OP_STORE, 224, 225, 249, 250, 251, 252, OP_RETURN, 254, 255}


def _producer_type(m, idx):
    """the result type of the instruction that computes idx: every value instruction's operand 2 is its result id, operand 1 its result type (the text patch
    knew the type from the disassembly's names; here it comes from the module)"""
    ts = {m.args(i)[0] for i in range(len(m.code)) if m.op(i) not in NOT_VALUE and len(m.args(i)) >= 2 and m.args(i)[1] == idx}
    if len(ts) != 1: raise ValueError(f'cull_private_memory: the index of a shared-array access has {len(ts)} candidate types, one expected')
    return ts.pop()

def _pointer_types(m):
    """{result id: (storage class, the type it points at)} for every OpTypePointer"""
    out = {}
    for i in m.find(OP_TYPEPOINTER):
        a = m.args(i)          # result id, storage class, type
        if len(a) >= 3: out[a[0]] = (a[1], a[2])
    return out

def _variables(m):
    """{result id: (the pointer type, the storage class)} for every OpVariable"""
    out = {}
    for i in m.find(OP_VARIABLE):
        a = m.args(i)          # result type, result id, storage class, [initializer]
        if len(a) >= 3: out[a[1]] = (a[0], a[2])
    return out

def _constants(m):
    """{result id: (the type, the value)} for every OpConstant and OpConstantFalse"""
    out = {}
    for i in m.find(OP_CONSTANT):
        a = m.args(i)          # result type, result id, value
        if len(a) >= 3: out[a[1]] = (a[0], a[2] & 0xffffffff)
    for i in m.find(OP_CONSTFALSE):
        a = m.args(i)
        if len(a) >= 2: out[a[1]] = (a[0], 0)
    return out

def _stores(m, var, value):
    """the indexes of the stores of the value into the variable"""
    return m.find(OP_STORE, lambda a: len(a) >= 2 and a[0] == var and a[1] == value)

def _block_returns(m, label):
    """the block of the label ends in an OpReturn (the instance is skipped there)"""
    starts = [i for i in range(len(m.code)) if m.op(i) == OP_LABEL and m.args(i)[0] == label]
    if not starts: return False
    for i in range(starts[0] + 1, len(m.code)):
        if m.op(i) == OP_LABEL: return False
        if m.op(i) == OP_RETURN: return True
    return False

def _skips(m, vis, int_zero, after):
    """the visibility is read after the store and compared with 0, and the branch that follows leads to a block that returns: the instance is skipped"""
    for i in m.find(OP_LOAD, lambda a: len(a) >= 3 and a[2] == vis):
        if i <= after: continue
        loaded = m.args(i)[1]
        cmps = m.find(OP_IEQUAL, lambda a: len(a) >= 4 and a[2] == loaded and a[3] == int_zero) + \
               m.find(OP_INOT_EQUAL, lambda a: len(a) >= 4 and a[2] == loaded and a[3] == int_zero)
        for c in cmps:
            res = m.args(c)[1]
            conds = [res] + [m.args(x)[1] for x in m.find(OP_LOGICAL_NOT, lambda a: len(a) >= 3 and a[2] == res)]
            for b in m.find(OP_BRANCH_COND, lambda a: len(a) >= 3 and a[0] in conds):
                if _block_returns(m, m.args(b)[1]) or _block_returns(m, m.args(b)[2]): return True
    return False

def _occlusion_comparison(m):
    """the index of the comparison of the depth the pyramid holds with the object's nearest depth (the fetch of the pyramid's level, its component, the comparison)"""
    fetches = [i for i in m.find(OP_FETCH) if len(m.args(i)) >= 6 and (m.args(i)[4] & 2)]
    if len(fetches) != 1: raise ValueError(f'cull_visibility: {len(fetches)} depth fetches, one expected')
    fetched = m.args(fetches[0])[1]
    ext = m.find(OP_EXTRACT, lambda a: len(a) >= 4 and a[2] == fetched and a[3] == 0)
    if len(ext) != 1: raise ValueError('cull_visibility: the fetch is not followed by one component')
    depth = m.args(ext[0])[1]
    cmps = m.find(OP_FORD_GE, lambda a: len(a) >= 4 and a[3] == depth) + m.find(OP_FUNORD_GE, lambda a: len(a) >= 4 and a[3] == depth)
    if len(cmps) != 1: raise ValueError('cull_visibility: the fetched depth is not compared once')
    return cmps[0]

@spv_post.binary
def cull_visibility(m):
    """The translated shader zeroes an instance's visibility after its occlusion loops whatever they found, so the indirect draws got 0 instances: the whole
    instanced scenery (the field tiles, the cliff walls, the shore, the grass, the furniture) was never rasterised and the empty G-buffer showed through as one
    flat grey. The store now zeroes the visibility only for an instance whose loops ran and found no visible texel; an instance that found one keeps it, and one
    the shader does not test keeps it too.
    The state the patch keeps (the 1st Chapter's %fixv): 0 the loops were not run, 2 they ran and no texel passed yet, 1 a texel passed.
    The sites are found by their structure: the occlusion comparison (the fetched depth against the object's nearest depth), the state variable it feeds, the
    bool it is stored in, the visibility variable the store after the loops zeroes, and the skip check that reads it."""
    ptr, var, const = _pointer_types(m), _variables(m), _constants(m)
    # the occlusion comparison: the level of the depth pyramid the shader fetches, the comparison of the fetched depth with the object's nearest depth
    cmp = _occlusion_comparison(m)
    cmp_result, bool_type = m.args(cmp)[1], m.args(cmp)[0]
    # the bool the comparison's result is stored in (the per texel visibility)
    bools = []
    for i in m.find(OP_STORE, lambda a: len(a) >= 2 and a[1] == cmp_result):
        p = m.args(i)[0]
        if p in var and var[p][1] == STORAGE_FUNCTION and ptr.get(var[p][0], (0, 0))[1] == bool_type: bools.append((i, p))
    if len(bools) != 1: raise ValueError('cull_visibility: the comparison is not stored in one bool')
    bool_store, bool_var = bools[0]
    # the int type: the type of the constant -1 (the state the shader stores when a texel passed)
    minus_ones = [c for c, (t, val) in const.items() if val == 0xffffffff]
    if len(minus_ones) != 1: raise ValueError(f'cull_visibility: {len(minus_ones)} int constants -1, one expected')
    minus_one = minus_ones[0]; int_type = const[minus_one][0]
    ints = [v for v, (t, s) in var.items() if s == STORAGE_FUNCTION and ptr.get(t, (0, 0))[1] == int_type]
    # the state variable: the int the shader stores -1 into when a texel passed and 0 when it looks for the next one
    zero = [c for c, (t, val) in const.items() if val == 0 and t == int_type]
    if len(zero) != 1: raise ValueError('cull_visibility: the int constant 0 is not unique')
    int_zero = zero[0]
    states = [v for v in ints if len(_stores(m, v, minus_one)) == 1 and _stores(m, v, int_zero)]
    if len(states) != 1: raise ValueError(f'cull_visibility: {len(states)} state variables, one expected')
    state = states[0]
    one = [c for c, (t, val) in const.items() if val == 1 and t == int_type]
    two = [c for c, (t, val) in const.items() if val == 2 and t == int_type]
    if len(one) != 1 or len(two) != 1: raise ValueError('cull_visibility: the int constants 1 and 2 are not there')
    int_one, int_two = one[0], two[0]
    # the visibility: the int the shader zeroes after the loops, and the skip check that reads it
    cands = []
    for v in ints:
        st = _stores(m, v, int_zero)
        if len(st) != 1 or st[0] <= cmp: continue        # zeroed once, and after the occlusion loops
        if not _skips(m, v, int_zero, st[0]): continue    # and read by a check that skips the instance
        cands.append((v, st[0]))
    if len(cands) != 1: raise ValueError(f'cull_visibility: {len(cands)} visibility variables, one expected')
    vis, vis_store = cands[0]
    # the store of the state that starts a pass of the loops (the first one before the texel that passed)
    entries = [i for i in _stores(m, state, int_zero) if i < _stores(m, state, minus_one)[0]]
    if not entries: raise ValueError('cull_visibility: the state is not reset before the texel that passed')
    entry, found = min(entries), _stores(m, state, minus_one)[0]
    # the init: the bool's store of false before the loops (the shader does not test every instance)
    inits = [i for i in m.find(OP_STORE, lambda a: len(a) >= 2 and a[0] == bool_var and a[1] in const and const[a[1]][0] == bool_type and const[a[1]][1] == 0) if i < cmp]
    if not inits: raise ValueError('cull_visibility: the bool is not reset before the loops')
    init = max(inits)
    # the new variable: an int in the function's own storage, declared with the function's other variables (the translator puts them after the function's first
    # label: a variable before it makes Turnip's compiler crash, so the new one goes where the module's own are)
    func = max(i for i in m.find(OP_FUNCTION) if i < vis_store)
    label = next(i for i in range(func + 1, len(m.code)) if m.op(i) == OP_LABEL)
    last = label; j = label + 1
    while j < len(m.code) and m.op(j) == OP_VARIABLE: last = j; j += 1
    fixv, fixa, fixb, fixc, fixd, fixe, fixf, fixg = [m.ident() for _ in range(8)]
    ins = spv_post.instruction
    var_type = var[vis][0]
    # the store of the visibility: zero it only when the loops ran and found nothing
    m.replace(vis_store, [ins(OP_LOAD, int_type, fixa, fixv), ins(OP_IEQUAL, bool_type, fixb, fixa, int_two),
                           ins(OP_LOAD, int_type, fixc, vis), ins(OP_SELECT, int_type, fixd, fixb, int_zero, fixc),
                           ins(OP_STORE, vis, fixd)])
    # the texel that passed: the state is 1 from now on
    m.insert(found + 1, [ins(OP_STORE, fixv, int_one)])
    # a pass of the loops starts: the state is 2 (ran, nothing passed yet) unless a texel passed already
    m.insert(entry + 1, [ins(OP_LOAD, int_type, fixe, fixv), ins(OP_IEQUAL, bool_type, fixf, fixe, int_one),
                         ins(OP_SELECT, int_type, fixg, fixf, int_one, int_two), ins(OP_STORE, fixv, fixg)])
    # before the loops: the state is 0 (they did not run)
    m.insert(init + 1, [ins(OP_STORE, fixv, int_zero)])
    # the new variable, with the function's other variables
    m.insert(last + 1, [ins(OP_VARIABLE, var_type, fixv, STORAGE_FUNCTION)])
    return m

@spv_post.binary
def cull_pyramid_read(m):
    """The occlusion test's comparison of the object's nearest depth with the depth the pyramid holds: an ordered comparison fails on a NaN, and the object is
    culled for those frames. The comparison becomes unordered, so a NaN depth counts as visible (the 1st Chapter's cull_pyramid_read, PR #411). The read itself is left
    as it is: the shader clamps its own coordinates, and the device this game runs on gave no error for it."""
    cmps = [i for i in m.find(OP_FORD_GE) if i == _occlusion_comparison(m)]
    if len(cmps) != 1: raise ValueError(f'cull_pyramid_read: {len(cmps)} ordered comparisons, one expected')
    c = m.code[cmps[0]]
    c[0] = (c[0] & 0xffff0000) | OP_FUNORD_GE
    return m

@spv_post.binary
def cull_private_memory(m):
    """The translated shader keeps each invocation's scratch in a workgroup-shared array of 11,264 uints (1,024 invocations x 11 words = 45,056 bytes), and every access is
    OpAccessChain <ptr workgroup uint> <array> <idx> with idx = invocation * 11 + word (the disassembly's every site is ((invocation * 44 + c) >> 2)). An Adreno has 32 KB of
    workgroup memory (the Switch: 48 KB) and what lies past it is corrupted: where a dispatch has more than about 740 instances, the far tiles' slice lies in the range past
    32 KB, the occlusion loops there read and write corrupt words, and the farthest terrain band stays culled (the 1st Chapter's cull_private_memory, PR #411; Eden's pull request
    4587 names the cause). The array becomes a Private array of 16 uints declared beside the workgroup one, indexed by the old index modulo 11: the invocation's own slice,
    in the invocation's private storage, with the same per-invocation semantics and no 32 KB workgroup use at all.
    The sites are found by their structure, not by an id: the one Workgroup variable whose type points at an OpTypeArray of 11,264 uints, the AccessChains with one index into
    it, and (the 1st Chapter's own guard) a refusal when the array is used in any other way than the declaration, the accesses and the module's metadata."""
    ptr, var, consts = _pointer_types(m), _variables(m), _constants(m)
    # the shared array: the one Workgroup variable whose type points at an OpTypeArray of 11,264 uints (45,056 bytes)
    cands = []
    for v, (t, s) in var.items():
        if s != STORAGE_WORKGROUP or t not in ptr: continue
        arrs = m.find(OP_TYPEARRAY, lambda a: len(a) >= 3 and a[0] == ptr[t][1])
        if len(arrs) == 1 and consts.get(m.args(arrs[0])[2], (0, None))[1] == 11264: cands.append(v)
    if len(cands) != 1: raise ValueError(f'cull_private_memory: {len(cands)} workgroup arrays of 11,264 uints, one expected')
    arr = cands[0]
    arrs = m.find(OP_TYPEARRAY, lambda a: len(a) >= 3 and a[0] == ptr[var[arr][0]][1])
    uint_t = m.args(arrs[0])[1]
    # every access: OpAccessChain <ptr workgroup uint> <array> <one index>
    sites = m.find(OP_ACCESS_CHAIN, lambda a: len(a) >= 4 and a[2] == arr)
    if not sites or any(len(m.args(i)) != 4 for i in sites):
        raise ValueError(f'cull_private_memory: {len(sites)} sites of the shared array, and every one one indexed, both expected')
    # the 1st Chapter's guard: the array must be used in no other way than its declaration, its accesses and the module's metadata
    uses = [i for i in range(len(m.code)) if arr in m.args(i)]
    decl = m.find(OP_VARIABLE, lambda a: len(a) >= 2 and a[1] == arr)
    meta = {OP_NAME, OP_MEMBERNAME, OP_ENTRYPOINT, OP_EXECMODE, OP_DECORATE, OP_DECORATEID, OP_DECORATESTR, OP_MEMBERDECORATESTR}
    extra = [i for i in uses if i not in decl and i not in sites and m.op(i) not in meta]
    if extra: raise ValueError(f'cull_private_memory: the shared array is used in a way this patch does not cover ({len(extra)} sites)')
    # the index's type (every site's index is produced as ((invocation * 44 + c) >> 2), an int here), and the remainder that fits it
    itype = _producer_type(m, m.args(sites[0])[3])
    if any(_producer_type(m, m.args(i)[3]) != itype for i in sites[1:]):
        raise ValueError('cull_private_memory: the shared array is indexed by more than one type')
    sig = m.find(OP_TYPEINT, lambda a: len(a) >= 3 and a[0] == itype)
    if len(sig) != 1: raise ValueError('cull_private_memory: the index type is not one OpTypeInt')
    rem = OP_SMOD if m.args(sig[0])[2] else OP_UMOD
    ins = spv_post.instruction
    c11, c16, arrt, parr, puint, parrv = (m.ident() for _ in range(6))
    # each access reads and writes the invocation's own slice: the pointer becomes private, the index its word within the slice (kept in place, so its users keep their id)
    mod = {}
    for i in sites:
        a = m.args(i)
        pm = m.ident()
        mod[i] = (a[3], pm)
        m.replace(i, [ins(OP_ACCESS_CHAIN, puint, a[1], parrv, pm)])
    for i in sorted(sites, reverse=True):
        idx, pm = mod[i]
        m.insert(i, [ins(rem, itype, pm, idx, c11)])
    # the private array of 16 uints and its types/constants, declared beside the workgroup one (before it, like the module's own declarations; last, because the
    # insert shifts the sites' indexes while the rewrite and the remainders must run on the module as it was scanned)
    m.insert(decl[0], [ins(OP_CONSTANT, itype, c11, 11), ins(OP_CONSTANT, uint_t, c16, 16),
                       ins(OP_TYPEARRAY, arrt, uint_t, c16), ins(OP_TYPEPOINTER, parr, STORAGE_PRIVATE, arrt),
                       ins(OP_TYPEPOINTER, puint, STORAGE_PRIVATE, uint_t), ins(OP_VARIABLE, parr, parrv, STORAGE_PRIVATE)])
    return m

@spv_post.binary
def cull_shader(m):
    return cull_pyramid_read(cull_private_memory(cull_visibility(m)))

SPIRV = {'37727c65c9440244': cull_shader}
