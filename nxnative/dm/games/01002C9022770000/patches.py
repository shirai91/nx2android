# Trails in the Sky 1st Chapter (01002C9022770000): the rewriter's patches of this game's own builds (dm/tools/patch_nso.py "Registered patches") and, below, the patch of one
# translated shader (SPIRV).
from nso_asm import ADRP, B

# Render size. The game keeps its sizes in a structure at main+0x7261b0, filled once by main+0x8540: +0x1b4 the scene's reference size, 1600x900 in both modes, from
# which it allocates its scene targets and by which it multiplies its dynamic resolution's factor (+0x210 / +0x200, 1.0 down to 0.5); +0x1cc the window, 1920x1080
# docked. The stub replaces the reference size's store (main+0x8688, str x9,[x8]) by the state page's render scale (+0x10, the host's DM_RENDER_SCALE, which the
# Android pause menu's Resolution row sets by android.tsv's DM_MENU_RES: Low 0.5, Auto 1.0, High 0.9): 1.0 or unset keeps 1600x900, a value from 0.75 up to 1.0 gives 1920x1080 (the docked
# window's own size: no upscale), a smaller one 1280x720. Read once, when the game starts: a change in the menu takes effect at the next start.
# Found on the AYN Odin 3 (2026-11-15) by writing the reference size in memory before the targets were made: 1920x1080 and 2560x1440 both drew a correct picture
# (2560x1440: GPU 19 ms a frame there, 40 fps), and a size written after the targets were made drew the scene cropped.
def render_size(ctx):
    here, emit = ctx.here, ctx.emit
    ctx.site(0x8688, '090100f9')
    start = here(); emit(ADRP(16, here(), ctx.state))
    for w in (0xb9401211,   # ldr w17,[x16,#0x10]        the render scale's bits (positive floats order as integers)
              0x34000171,   # cbz w17,store (+44)        unset
              0x52a7f010,   # mov w16,#0x3f800000        1.0
              0x6b10023f,   # cmp w17,w16
              0x5400010a,   # b.ge store (+32)
              0xd280f009,   # mov x9,#1920
              0xf2c08709,   # movk x9,#1080,lsl #32
              0x52a7e810,   # mov w16,#0x3f400000        0.75
              0x6b10023f,   # cmp w17,w16
              0x5400006a,   # b.ge store (+12)
              0xd280a009,   # mov x9,#1280
              0xf2c05a09,   # movk x9,#720,lsl #32
              0xf9000109):  # store: str x9,[x8]         (original)
        emit(w)
    emit(B(here(), 0x868c))
    ctx.put(0x8688, [B(0x8688, start)], 'scene reference size by the host render scale (1.0 = the game\'s 1600x900, High 1920x1080, Low 1280x720)'); ctx.render_scale = 1

FRAME_STEP = {'8963a531fa7f27a52323081ace0f2f6e': render_size}

# ---- The instance culling compute shader, 43c2b838b11bc19c (SPIRV: a patch of the translated shader's spirv-dis text, dm/android/tools/shader_vk.py spirv_patches) ----
# Each invocation tests one instance of the instanced scenery (furniture, grass, trees, the cliff walls) against the view and the depth pyramid and writes the indirect
# draws' instance lists. Four things were wrong with it on the AYN Odin 3 (Adreno 830, Turnip), each found there on 2026-11-15; a part raises ValueError when the text
# is not the one it was written for (the ids are the translator's for this shader: a translator change that renumbers them needs the numbers read again).
import re

def cull_visibility(t):
    """The translated shader zeroes an instance's visibility after its occlusion loops whatever they found (OpStore %94 %int_0 in the block after the loops), so the
    indirect draws got 0 instances: with counters added to the shader, of 452 instances in a room 70 reached the loops, 68 found a visible texel, none survived, and the
    furniture was missing. The store now zeroes the visibility only for an instance whose loops ran and found no visible texel. One the shader does not test (it skips
    the loops: a large or near piece, the cliff walls outdoors) keeps it: culled too, those pieces flickered as they went in and out of the test. The cause in the
    translator (its control flow after a break out of nested loops) is not found.
    %fixv: 0 the loops were not run, 2 they ran and no texel passed yet, 1 a texel passed."""
    s = lambda x: [k for k, l in enumerate(t) if l.strip() == x]
    stores, inits, found, entry = s('OpStore %94 %int_0'), s('OpStore %242 %false'), s('OpStore %552 %int_n1'), s('OpStore %552 %int_0')   # found: in the loops, a visible texel; entry: an outer loop pass starts
    lastvar = max((k for k, l in enumerate(t[:inits[0]]) if '= OpVariable %_ptr_Function_' in l), default=-1) if inits else -1
    if len(stores) != 1 or not inits or len(found) != 1 or not entry or lastvar < 0 or any('%fixv' in l for l in t) or not any('OpGroupNonUniform' in l for l in t):
        raise ValueError('cull_visibility: not the expected shader text')
    t = list(t); k = stores[0]
    t[k:k + 1] = ['%fixa = OpLoad %int %fixv', '%fixb = OpIEqual %bool %fixa %int_2', '%fixc = OpLoad %int %94', '%fixd = OpSelect %int %fixb %int_0 %fixc', 'OpStore %94 %fixd']
    t[found[0]:found[0] + 1] = [t[found[0]], 'OpStore %fixv %int_1']
    for e in sorted((x for x in entry if x < found[0]), reverse=True)[-1:]:   # the first one: the outer loop's header pass
        t[e:e + 1] = [t[e], '%fixe = OpLoad %int %fixv', '%fixf = OpIEqual %bool %fixe %int_1', '%fixg = OpSelect %int %fixf %int_1 %int_2', 'OpStore %fixv %fixg']
    t.insert(inits[0] + 1, 'OpStore %fixv %int_0')
    t.insert(lastvar + 1, '%fixv = OpVariable %_ptr_Function_int Function')
    return t

def cull_private_memory(t):
    """The shader declares 45,056 bytes of workgroup-shared memory, 44 for each of its 1,024 invocations, and each invocation uses only its own slice (every access is
    OpAccessChain <shared> ((invocation index * 44 + k) >> 2)). An Adreno has 32 KB (the Switch: 48 KB) and what lies past it is corrupted: outdoors, where a dispatch
    has more than about 740 instances, pieces of the scenery were there in one frame and gone in the next, or drawn out of place (12 frames of a still camera: about
    9,000 of 129,600 samples changed from one frame to the next, under 50 with this). Eden's pull request 4587 names the cause for this game. The array becomes a
    private array of the invocation's own slice: 16 words (the slice is 11), indexed by the old index modulo 11."""
    var = next((l for l in t if 'OpVariable %_ptr_Workgroup_' in l), None)
    if var is None or any('%pm_arr' in l for l in t): raise ValueError('cull_private_memory: no workgroup-shared array')
    name = var.split('=')[0].strip()
    acc = [i for i, l in enumerate(t) if re.search(r'OpAccessChain %_ptr_Workgroup_uint ' + re.escape(name) + r' %\d+\s*$', l)]
    if not acc or any(name + ' ' in l or l.rstrip().endswith(name) for i, l in enumerate(t) if i not in acc and 'OpVariable' not in l and 'OpName' not in l and 'OpDecorate' not in l and 'OpEntryPoint' not in l):
        raise ValueError('cull_private_memory: the shared array is used in a way this patch does not cover')
    t = list(t); ins = []
    for n, i in enumerate(acc):
        idx = t[i].split()[-1]
        t[i] = re.sub(r'OpAccessChain %_ptr_Workgroup_uint ' + re.escape(name) + r' %\d+', f'OpAccessChain %pm_ptr_uint %pm_arr %pm_m{n}', t[i])
        ins.append((i, f'%pm_m{n} = OpSMod %int {idx} %pm_11'))
    for i, l in sorted(ins, reverse=True): t.insert(i, l)
    k = next(i for i, l in enumerate(t) if l.strip() == var.strip())
    t[k:k + 1] = [t[k], '%pm_11 = OpConstant %int 11', '%pm_n = OpConstant %uint 16', '%pm_arrt = OpTypeArray %uint %pm_n', '%pm_parr = OpTypePointer Private %pm_arrt',
                  '%pm_ptr_uint = OpTypePointer Private %uint', '%pm_arr = OpVariable %pm_parr Private']
    return t

def cull_pyramid_read(t):
    """The occlusion test's read of the depth pyramid. The object's nearest depth is a maximum over its box corners; a corner on the camera's plane (w = 0, as the camera
    walks into a box: a tree's crown) gives infinities and NaNs. The Switch's GPU maximum and minimum ignore a NaN operand; GLSL's FMax / FMin may return it, the ordered
    comparison with the pyramid then fails and the object is culled for those frames (a tree gone and back while walking towards it). Every FMax / FMin becomes NMax /
    NMin and the comparison is unordered: a NaN depth counts as visible. The read is also guarded: the pyramid is 800x450 with every level halved and rounded down, the
    shader reads texel (pixel >> level), which can lie past a level (799 >> 6 = 12 in a level 12 wide), and Vulkan leaves such a read undefined; it now gives 0, the
    far plane of the game's reversed depth, which hides nothing. (The shader clamps its coordinates itself, so the guard only matters if the size the game passes
    differs from the texture's.)"""
    f = [k for k, l in enumerate(t) if ' = OpImageFetch %v4float ' in l and ' Lod ' in l]
    if len(f) != 1 or any('%hzsz' in l for l in t): raise ValueError('cull_pyramid_read: expected one level fetch')
    m = re.match(r'\s*(%\w+) = OpImageFetch %v4float (%\w+) (%\w+) Lod (%\w+)', t[f[0]]); res, img, coord, lod = m.groups()
    x = re.match(r'\s*(%\w+) = OpCompositeExtract %float ' + re.escape(res) + r' 0\s*$', t[f[0] + 1])
    if not x: raise ValueError('cull_pyramid_read: the fetch is not followed by its component')
    glsl = next((m.group(1) for m in (re.match(r'\s*(%\w+) = OpExtInstImport "GLSL.std.450"', l) for l in t) if m), None)
    names = {l.split('=')[0].strip() for l in t if '=' in l}
    if glsl is None or not {'%v2int', '%int', '%bool', '%float_0', '%int_0', '%int_1'} <= names: raise ValueError('cull_pyramid_read: a type or constant is missing')
    tv = max(k for k, l in enumerate(t) if re.match(r'\s*%\w+ = OpConstant ', l))
    decl = ([] if '%v2bool' in names else ['%v2bool = OpTypeVector %bool 2']) + ['%hz_zero2 = OpConstantComposite %v2int %int_0 %int_0', '%hz_one2 = OpConstantComposite %v2int %int_1 %int_1']
    body = [f'%hzsz = OpImageQuerySizeLod %v2int {img} {lod}',
            f'%hzlt = OpSLessThan %v2bool {coord} %hzsz', f'%hzge = OpSGreaterThanEqual %v2bool {coord} %hz_zero2',
            '%hzin1 = OpAll %bool %hzlt', '%hzin2 = OpAll %bool %hzge', '%hzin = OpLogicalAnd %bool %hzin1 %hzin2',
            '%hzmax = OpISub %v2int %hzsz %hz_one2', f'%hzcl = OpExtInst %v2int {glsl} SClamp {coord} %hz_zero2 %hzmax',
            f'{res} = OpImageFetch %v4float {img} %hzcl Lod {lod}', f'%hzr = OpCompositeExtract %float {res} 0',
            f'{x.group(1)} = OpSelect %float %hzin %hzr %float_0']
    cmpl = t[f[0] + 2]
    if ' = OpFOrdGreaterThanEqual %bool ' not in cmpl or x.group(1) not in cmpl: raise ValueError('cull_pyramid_read: the comparison after the fetch is not the expected one')
    out = t[:f[0]] + body + [cmpl.replace('OpFOrdGreaterThanEqual', 'OpFUnordGreaterThanEqual')] + t[f[0] + 3:]
    out = [re.sub(r'(OpExtInst %\w+ ' + re.escape(glsl) + r') F(Max|Min) ', r'\1 N\2 ', l) for l in out]
    return out[:tv + 1] + decl + out[tv + 1:]

def cull_detail_distance(t):
    """The Android pause menu's Detail distance row (android.tsv: env DM_DETAIL_DIST=1). The shader compares each object's distance to the camera (the square root of
    variable %136) with the object's minimum and maximum distances: the level-of-detail ranges (a tree's near and far versions) and the distance at which small objects
    are dropped. The squared distance is multiplied by the square of the value the host keeps in its support buffer, entry 72 of the render scale array
    (gfx_vk_shader.c dm_detail_dist_set): the inverse of the row's factor, 1 for the game's own distances. With 1/1.5 an object 150 m away is treated as 100 m away."""
    st = [k for k, l in enumerate(t) if re.match(r'\s*OpStore %136 %\w+\s*$', l)]
    names = {l.split('=')[0].strip() for l in t if '=' in l}
    if len(st) != 1 or any('%ddist_' in l for l in t) or not {'%_ptr_Uniform_float', '%support_buffer', '%float', '%int'} <= names: raise ValueError('cull_detail_distance: not the expected shader text')
    val = t[st[0]].split()[2]; ind = re.match(r'\s*', t[st[0]]).group(0)
    body = [ind + '%ddist_p = OpAccessChain %_ptr_Uniform_float %support_buffer %ddist_5 %ddist_72', ind + '%ddist_s = OpLoad %float %ddist_p',
            ind + '%ddist_q = OpFMul %float %ddist_s %ddist_s', ind + '%ddist_v = OpFMul %float ' + val + ' %ddist_q', ind + 'OpStore %136 %ddist_v']
    out = t[:st[0]] + body + t[st[0] + 1:]
    tv = max(k for k, l in enumerate(out) if re.match(r'\s*%\w+ = OpConstant ', l))
    return out[:tv + 1] + ['%ddist_5 = OpConstant %int 5', '%ddist_72 = OpConstant %int 72'] + out[tv + 1:]

def cull_shader(t): return cull_detail_distance(cull_pyramid_read(cull_private_memory(cull_visibility(t))))

SPIRV = {'43c2b838b11bc19c': cull_shader}
