#!/usr/bin/env python3
# Android shader build: a game's Switch shaders (the package directory shader_split.py writes) → Vulkan SPIR-V + replay reflection, packed into one bundle.
# The same translator as the Apple chain (dm/gfx/tools/dmsh_xlate_bin, Ryujinx's translator with TargetApi.Vulkan) and the same jobs as dmsh_build.py
# (control section / code area / c1 split, the VertexA + VertexB merge, the profile's answer table), but no MSL: Vulkan takes the SPIR-V as it is.
# Geometry shaders are translated as Vulkan geometry shaders (the Apple chain emulates them in compute kernels).
#   DM_GAME=<id> shader_vk.py <package dir> <out bundle> [--jobs N] [--keys k1,k2] [--xlate <dmsh_xlate.dll>] [--capture <dir>] [--zstd <level>]
# --capture: shaders whose code no game file holds, saved on the device by the Vulkan backend (gfx_vk_shader.c vk_shader_missing: <key>.bin = "DMSC", u32 control length, u32 code length,
#   control section, code area), translated beside the package's; a file whose content does not hash to its name, or whose key the package has, is left out
# Bundle (read by the Vulkan backend, dm/android/gfx_vk_shader.c): dmsv.py has the format and writes it. Each shader's SPIR-V and its binary reflection (refl_bin, below);
#   version 2, the SPIR-V of each module zstd-compressed (--zstd, default dmsv.LEVEL; 0 or a Python without zstd: version 1, uncompressed).
import argparse, collections, concurrent.futures as cf, hashlib, json, os, re, struct, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent.parent / 'gfx/tools'
sys.path[:0] = [str(TOOLS), str(HERE)]
import dmsh_facts, dmsh_chain, dmsv, shader_products   # noqa: E402  (DM_GAME picks the profile)
from shader_split import glslc_stage   # the package reader also recognizes headerless compute code

ap = argparse.ArgumentParser()
ap.add_argument('pkg'); ap.add_argument('out')
ap.add_argument('--jobs', type=int, default=os.cpu_count() or 8)
ap.add_argument('--keys', default='')
ap.add_argument('--dotnet', default=os.environ.get('DOTNET', 'dotnet'))
ap.add_argument('--report', help='write the expected and omitted shader inventory as JSON')
ap.add_argument('--capture', help='a folder of shaders captured at run time on the device (<key>.bin)')
ap.add_argument('--zstd', type=int, default=dmsv.LEVEL, help=f'zstd level of the SPIR-V in the bundle (default {dmsv.LEVEL}); 0: version 1, uncompressed')
ap.add_argument('--xlate', help="the translator (dmsh_xlate.dll) to run (default: the iOS chain's dm/gfx/tools/dmsh_xlate_bin; the porter builds one from the in-repo source)")
a = ap.parse_args()

GAME = dmsh_chain.load_game()
FACTS = dmsh_facts.load()
MAP = str(GAME.table('shader_map.tsv'))
XLATE_DLL = a.xlate or str(TOOLS / 'dmsh_xlate_bin/dmsh_xlate.dll')
STAGES = {'vs': 0, 'ps': 1, 'gs': 2, 'hs': 3, 'ds': 4, 'cs': 5}   # NVN stage numbers
# Storage buffers are bound from a 256-byte boundary below their address (gfx_vk_exec.c SB_ALIGN, the same number): Vulkan's largest minStorageBufferOffsetAlignment, so a multiple of
# every device's; the translator offsets every storage access by the address's distance from that boundary (Program.cs QueryHostStorageBufferOffsetAlignment)
SB_ALIGN = 256
# Maxwell's warps are 32 lanes; Adreno runs 64 or 128 (a wave's size is the driver's choice per pipeline). Translated for a group above 32 (Program.cs QueryHostSubgroupSize),
# the translator emulates 32-lane warps inside it: lane id & 31, the ballot word of the lane's own 32, shuffles inside its 32, votes by ballot. Its 128 path picks the word by
# lane id >> 5, so it holds for 64 too. At 32 the warp code read only the first 32 lanes' ballot (Tears of the Kingdom: 32 shaders vote, ballot or shuffle)
SUBGROUP = int(GAME.get("android_subgroup_size", "128"))
# Adreno rounds a gather coordinate that sits on a texel edge the other way from Maxwell, so a gather can pick the wrong 2x2 texels. Eden moves gather coordinates
# 1/512 texel on Qualcomm and Turnip (#4565, TotK's large dark squares); Ryujinx's TexturePass does the same with 8 bits (Program.cs QueryHostGatherBiasPrecision).
# Tears of the Kingdom on the Adreno 830 drew dark boxes of whole 16x16 tiles in its tiled lighting (607 shaders gather, 38 of them compute): in 2 of 12 camera
# steps at the Great Sky Island pond and every frame at the Temple of Time's stone circle; with the bias, 0 in 24
GATHER_BIAS = 8

def records(d):
    r = [m.start() for m in re.finditer(b'BTCDBTCD', d)]
    return [d[x:(r[k + 1] if k + 1 < len(r) else len(d))] for k, x in enumerate(r)]

def captured(d):
    """The run-time captures in folder d (--capture): {key: (stage, record in the package's form)}"""
    out = {}
    for nm in sorted(os.listdir(d)) if d and os.path.isdir(d) else []:
        if not re.fullmatch(r'[0-9a-f]{16}\.bin', nm): continue
        b = open(os.path.join(d, nm), 'rb').read()
        if len(b) < 12 or b[:4] != b'DMSC': print(f'[RED] capture {nm}: not a captured shader', flush=True); continue
        nctl, ncode = struct.unpack_from('<2I', b, 4); ctl, code = b[12:12 + nctl], b[12 + nctl:12 + nctl + ncode]
        if len(code) != ncode or len(ctl) != nctl or ctl[:4] != bytes.fromhex('34127698'): print(f'[RED] capture {nm}: truncated', flush=True); continue
        if hashlib.sha256(ctl + code).hexdigest()[:16] != nm[:16]: print(f'[RED] capture {nm}: its content does not hash to its name', flush=True); continue
        st = glslc_stage(ctl, code)
        if not st: print(f'[RED] capture {nm}: cannot determine its shader stage', flush=True); continue
        rec = bytearray(b'BTCDBTCD' + bytes(0x30)); struct.pack_into('<2I', rec, 0x28, len(ctl), len(code)); rec += code + ctl
        if rec.find(bytes.fromhex('34127698')) != 0x38 + len(code): print(f'[RED] capture {nm}: the control magic appears in its code area', flush=True); continue   # job_for splits there
        out[nm[:16]] = (st, bytes(rec))
    return out

def vertex_ab(key, code, ctl):   # dmsh_build.py vertex_ab: a VertexA + VertexB program merged as Ryujinx does
    if len(code) < 0x80 or len(ctl) < 0x760 or code[:4] != bytes.fromhex('78563412'): return None
    w0 = struct.unpack_from('<I', code, 0x30)[0]
    if w0 & 0x1f != 1 or (w0 >> 10) & 0xf != 0: return None
    a_sph, a_len, _, b_sph, b_len, _ = struct.unpack_from('<6I', ctl, 0x748)
    if not any((a_sph, a_len, b_sph, b_len)): return None
    bw0 = struct.unpack_from('<I', code, b_sph)[0] if 0x80 <= b_sph <= len(code) - 0x50 else 0
    if a_sph != 0x30 or b_sph != struct.unpack_from('<I', ctl, 0x700)[0] + 0x30 or bw0 & 0x1f != 1 or (bw0 >> 10) & 0xf != 1 or (a_len - 0x50) % 32 or (b_len - 0x50) % 32 or b_sph + b_len > len(code):
        return None
    sa, sb = list(struct.unpack_from('<20I', code, a_sph)), list(struct.unpack_from('<20I', code, b_sph))
    sph = [sb[0] | sa[0] & ~(0xf << 10)] + [max(sa[i] & 0xffffff, sb[i] & 0xffffff) | sb[i] & ~0xffffff for i in (1, 2, 3)] + [sb[4]] + [sa[i] | sb[i] for i in range(5, 20)]
    av = bytearray(code[a_sph + 0x50:a_sph + a_len]); b = code[b_sph + 0x50:b_sph + b_len]
    for o in range(0, len(av), 8):
        if o % 32 == 0: continue
        ins = struct.unpack_from('<Q', av, o)[0]
        if ins >> 52 == 0xe30 and ins & 0x1f == 0xf: struct.pack_into('<Q', av, o, 0xe24 << 52 | ((len(av) - (o + 8)) & 0xffffff) << 20 | ins & 0xfffff)
    return code[:0x30] + struct.pack('<20I', *sph) + bytes(av) + b, code[b_sph + b_len:]

def xfb_layout(key, ctl):
    """A vertex shader's transform feedback as its control section declares it (where the NVN driver reads it; dmsh_build.py tf_layout reads the one-output form): +0x6dc u16 stride of
    each buffer, +0x6e8 u8 number of outputs it captures, +0xdc + 0x80 b those outputs (0x20 + n = generic output n), +0x4dc + 0x80 b their component counts (u8 each); the outputs
    follow each other in the buffer (up to 9 outputs interleaved in one buffer, stride 144, in the game it was found with). → the translator's job "xfb": per buffer None or
    {stride, words}, words = the output word at each 4-byte position (Maxwell's numbering, 0x20 + 4 n + component); None without feedback or in a form not known"""
    strides = struct.unpack_from('<4H', ctl, 0x6dc)
    if not any(strides): return None
    out = []
    for b in range(4):
        if not strides[b]: out.append(None); continue
        n = ctl[0x6e8 + b]; outs, comps = ctl[0xdc + 0x80 * b:0xdc + 0x80 * b + n], ctl[0x4dc + 0x80 * b:0x4dc + 0x80 * b + n]
        words = [0x20 + 4 * (o - 0x20) + c for o, k in zip(outs, comps) for c in range(k)]
        if not n or any(not 0x20 <= o < 0x40 for o in outs) or any(not 1 <= k <= 4 for k in comps) or 4 * len(words) != strides[b]:
            print(f'[RED] {key}: transform feedback buffer {b} in a form not known (stride {strides[b]}, outputs {list(outs)}, components {list(comps)}): translated without capture', flush=True)
            return None
        out.append({'stride': strides[b], 'words': words})
    return out

def gs_variant_key(key, t):   # the bundle key of a geometry shader translated for input primitive t (0 points, 1 lines, 2 lines with adjacency, 4 triangles with adjacency)
    return key ^ ((t + 1) * 0x9E3779B97F4A7C15 & 0xFFFFFFFFFFFFFFFF)

def job_for(key, stage, rec, w):   # dmsh_build.py job_for; transform feedback is captured natively (xfb_layout), from the vertex or the geometry stage
    ci = rec.find(bytes.fromhex('34127698')); code = rec[0x38:ci]
    c1o, c1t = struct.unpack_from('<2I', rec, ci + 0x700)
    c1 = rec[0x38 + c1o:0x38 + c1t]
    ab = vertex_ab(key, code, rec[ci:]) if stage == 'vs' else None
    if ab: code, c1 = ab
    if stage != 'cs': code = code[0x30:]
    elif code[:4] == bytes.fromhex('79563412'): code = code[0x100:]
    cp = os.path.join(w, key + '.bin'); open(cp, 'wb').write(code)
    xfb = xfb_layout(key, rec[ci:]) if stage in ('vs', 'gs') else None   # a geometry shader captures what it emits (Xenoblade Chronicles 3's particle update, 293ecebc0136519b)
    j = {'key': key, 'stage': stage, 'code': cp, 'tex': {}, 'img': {}, 'attr': {}, 'fout': {}, 'c1': c1.hex(), 'debug': False, 'reduced': True, 'sb_align': SB_ALIGN, 'subgroup': SUBGROUP, 'gather_bias': GATHER_BIAS, 'gs_passthrough': False, 'draw_parameters': True, 'viewport_mask': False, **({'xfb': xfb} if xfb else {})}
    if stage == 'cs':   # Runtime captures have no facts-table row; their control section still declares the workgroup.
        x, y, z, shared = struct.unpack_from('<4I', rec, ci + 0x748)
        j.update(local=[x, y, z], shared=(shared + 15) // 16 * 16)
    # GLSLC marks a fragment shader declared layout(early_fragment_tests) at control +0x718 (bit 0); NVN then sets the 3D engine's FORCE_EARLY_FRAGMENT_TESTS, which
    # the code area does not carry. Monster Hunter Rise's occlusion culling draws boxes over a 128x64 depth and stores a flag per visible box: tested after the shader,
    # every box's fragments ran and flagged it (15 ms a frame in Kamura village on Adreno 650)
    if stage == 'ps' and len(rec) >= ci + 0x71c and struct.unpack_from('<I', rec, ci + 0x718)[0] & 1: j['early_z'] = True
    if key in FACTS: j.update(FACTS[key])
    else: j['no_facts'] = True
    return j, c1

# ---- SPIR-V reflection: descriptor bindings with the translator's names, decoded as dmsh_build.py reflect decodes the MSL names ----
OP_NAME, OP_DECORATE, OP_VARIABLE, OP_TYPE_POINTER, OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY, OP_TYPE_IMAGE, OP_TYPE_SAMPLED_IMAGE, OP_TYPE_SAMPLER, OP_TYPE_STRUCT, OP_CONSTANT, OP_EXECUTION_MODE = 5, 71, 59, 32, 28, 29, 25, 27, 26, 30, 43, 16
DEC_BINDING, DEC_SET, DEC_BLOCK, DEC_BUFFER_BLOCK, DEC_NONWRITABLE = 33, 34, 2, 3, 24
SC_UNIFORM_CONSTANT, SC_UNIFORM, SC_STORAGE_BUFFER = 0, 2, 12
def spirv_bindings(spv):
    w = struct.unpack(f'<{len(spv) // 4}I', spv)
    names, deco, types, consts, vars_ = {}, collections.defaultdict(dict), {}, {}, []
    i = 5
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xffff; ins = w[i:i + n]
        if op == OP_NAME: names[ins[1]] = bytes(struct.pack(f'<{n - 2}I', *ins[2:])).split(b'\0')[0].decode()
        elif op == OP_DECORATE: deco[ins[1]][ins[2]] = ins[3] if n > 3 else 1
        elif op in (OP_TYPE_POINTER,): types[ins[1]] = ('ptr', ins[2], ins[3])
        elif op == OP_TYPE_ARRAY: types[ins[1]] = ('array', ins[2], ins[3])
        elif op == OP_TYPE_RUNTIME_ARRAY: types[ins[1]] = ('rarray', ins[2])
        elif op == OP_TYPE_IMAGE: types[ins[1]] = ('image', ins[3], ins[7], ins[5], ins[6], ins[8])   # dim, sampled (1 sampled, 2 storage), arrayed, multisampled, image format
        elif op == OP_TYPE_SAMPLED_IMAGE: types[ins[1]] = ('sampled', ins[2])
        elif op == OP_TYPE_SAMPLER: types[ins[1]] = ('sampler',)
        elif op == OP_TYPE_STRUCT: types[ins[1]] = ('struct',)
        elif op == OP_CONSTANT and n == 4: consts[ins[2]] = ins[3]
        elif op == OP_VARIABLE: vars_.append((ins[2], ins[1], ins[3]))   # id, pointer type, storage class
        i += n
    out = []
    for vid, pt, sc in vars_:
        d = deco.get(vid, {})
        if DEC_BINDING not in d: continue
        t = types.get(types[pt][2]) if pt in types else None; tid = types[pt][2] if pt in types else None
        count = 1
        while t and t[0] in ('array', 'rarray'):
            count = consts.get(t[2], 1) if t[0] == 'array' else 0; tid = t[1]; t = types.get(tid)
        if sc == SC_UNIFORM: dt = 'ssbo' if deco.get(tid, {}).get(DEC_BUFFER_BLOCK) else 'ubo'
        elif sc == SC_STORAGE_BUFFER: dt = 'ssbo'
        elif t and t[0] == 'sampled': dt = 'tbuf' if types.get(t[1], ('',))[1:2] == (5,) else 'sampled'   # Dim 5 = Buffer: a uniform texel buffer
        elif t and t[0] == 'image': dt = 'storage_tbuf' if t[1] == 5 and t[2] == 2 else 'storage' if t[2] == 2 else 'tbuf' if t[1] == 5 else 'image'
        elif t and t[0] == 'sampler': dt = 'sampler'
        else: dt = '?'
        img = t if t and t[0] == 'image' else types.get(t[1]) if t and t[0] == 'sampled' else None
        out.append({'name': names.get(vid, ''), 'set': d.get(DEC_SET, 0), 'binding': d[DEC_BINDING], 'type': dt, 'count': count,
                    'dim': img[1] if img else None, 'arrayed': img[3] if img else 0, 'ms': img[4] if img else 0, 'fmt': img[5] if img else 0})
    local = None
    i = 5
    scalars = {}   # type id → ('f' | 'i' | 'u')
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xffff
        if op == OP_EXECUTION_MODE and w[i + 2] == 17: local = list(w[i + 3:i + 6])   # LocalSize
        elif op == 21: scalars[w[i + 1]] = 'i' if w[i + 3] else 'u'                  # OpTypeInt width signedness
        elif op == 22: scalars[w[i + 1]] = 'f'                                        # OpTypeFloat
        elif op == 23: scalars[w[i + 1]] = scalars.get(w[i + 2], 'f')                 # OpTypeVector: its component's kind
        i += n
    # the stage's interface: Input (1) / Output (3) variables with a Location → masks of all, signed integer and unsigned integer locations
    io = {1: [0, 0, 0], 3: [0, 0, 0]}
    for vid, pt, sc in vars_:
        if sc not in io or 30 not in deco.get(vid, {}): continue   # 30 = Location
        loc = deco[vid][30]
        if loc >= 32: continue
        t = types.get(pt); tid = t[2] if t else None
        while tid in types and types[tid][0] in ('array', 'rarray'): tid = types[tid][1]
        k = scalars.get(tid, 'f')
        io[sc][0] |= 1 << loc
        if k == 'i': io[sc][1] |= 1 << loc
        if k == 'u': io[sc][2] |= 1 << loc
    return out, local, io

# Handle decoding: as dmsh_build.py decode_tex / decode_ubo_tex / reflect (names: <stage>_t<type>_cb<slot>_<handle>, <stage>_c<n>, <stage>_s<n>, <stage>_i…)
TEX_BASE, IMG_BASE, IMG_SLOTS, SEPTEX_BASE, SEPTEX_SLOT0, SAMP_BASE = 0x20, 0x120, 8, 0x168, 128, 0x568
# NVN geometry c0 uses 0x410 + 16 * slot. NieR's IndexBuildIndependent GS reads
# slots 0 and 2 there to write the later colour draws' indirect indices/counts.
SSBO = {'vs': 0x110, 'gs': 0x410, 'cs': 0x310, 'ps': 0x510}
def classify(stage, b, res):
    nm = b['name']; e = dict(b)
    # Dynamic pool arrays use driver c2 slot indices (as the Metal backend).
    if GAME.get('android_dynamic_slots', 'off') == 'on' and b['type'] in ('image', 'sampler') and b['count'] == 32 and re.fullmatch(r'[a-z]+_[ts]\w*_cb0_40000000', nm):
        e.update(kind='texd' if b['type'] == 'image' else 'sampd'); return e
    m = re.fullmatch(r'[a-z]+_c(\d+)', nm)
    if m:
        n = int(m.group(1))
        e.update(kind='cbuf', reg=n - 3) if 3 <= n <= 16 else e.update(kind='c1' if n == 1 else 'drvcb', bank=n); return e
    if nm == 'support_buffer': e.update(kind='support'); return e
    m = re.fullmatch(r'[a-z]+_s(\d+)', nm)
    if m:
        idx = int(m.group(1)); sb = sorted(res.get('sbufs', []), key=lambda x: x['binding'])
        s = sb[idx] if idx < len(sb) else None; off = s['cb_offset'] * 4 if s and s['cb_slot'] == 0 else None; base = SSBO.get(stage)
        e.update(kind='sbuf', rw=bool(s and s['flags'] & 1))   # Ryujinx BufferUsageFlags.Write = 1 << 0
        if stage == 'cs' and off is not None and 0x210 <= off < 0x310 and (off - 0x210) % 16 == 0: e.update(kind='ubo_ptr', reg=(off - 0x210) // 16)
        elif s and s['cb_slot'] >= 3: e.update(kind='sbuf_ubo', reg=s['cb_slot'] - 3, off=s['cb_offset'] * 4)
        else: e['reg'] = (off - base) // 16 if off is not None and base is not None and 0 <= off - base < 0x100 and (off - base) % 16 == 0 else None
        return e
    # Textures and images, named by the translator <stage>_<t|i><type>_<cb<slots>|tcb>_<handle> (dmsh_build.py decode_tex / decode_ubo_tex): tcb = the driver's handle table c2;
    # cb<slots> = texture buffer slot in the low 16 bits, sampler buffer slot + 1 in the high 16 bits. Handle: word offsets o0 (bits 0-13), o1 (14-27), type (28-31):
    # 0 one combined handle (the sampler is the texture handle's high bits), 1 the sampler handle in another word, 3 a constant sampler ID in o1.
    # Vulkan binds each as a combined image sampler: 'tex' / 'tbuf' slots name the texture, samp_* where the sampler comes from.
    m = re.match(r'[a-z]+_([ti])\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)', nm)
    if m:
        kind = m.group(1); slots = int(m.group(2)) if m.group(2) else -1; h = int(m.group(3), 16)
        tb = (slots & 0xffff) if slots >= 0 else 2; sb = ((slots >> 16) - 1) if slots >= 0 and (slots >> 16) else tb
        o0, o1, ty = h & 0x3fff, (h >> 14) & 0x3fff, h >> 28
        if tb == 2:   # the driver's handle table c2
            def slot(word, base, cb_): return (word * 4 - base) // 8 if cb_ == 2 and word * 4 >= base and (word * 4 - base) % 8 == 0 else None
            words = [o0] + ([o1] if ty else [])
            if kind == 'i':
                e.update(kind='img', reg=next((slot(x, IMG_BASE, tb) for x in words if IMG_BASE <= x * 4 < IMG_BASE + 8 * IMG_SLOTS and slot(x, IMG_BASE, tb) is not None), None))
                if b['type'] == 'storage_tbuf': e['kind'] = 'img_tbuf'
                return e
            if b['count'] > 1 or b['count'] == 0:   # an array of c2 handles indexed at run time: element k = the handle at off + 8k
                e.update(kind='tex_c2a', off=o0 * 4 if ty == 0 else None); return e
            tw = [o0] if ty == 3 else words
            t = next((slot(x, TEX_BASE, tb) for x in tw if slot(x, TEX_BASE, tb) is not None and x * 4 < SEPTEX_BASE), None)
            if t is None: t = next((SEPTEX_SLOT0 + slot(x, SEPTEX_BASE, tb) for x in tw if slot(x, SEPTEX_BASE, tb) is not None and x * 4 < SAMP_BASE), None)
            e.update(kind='tbuf' if b['type'] == 'tbuf' else 'tex', reg=t)
            if ty == 3: e['samp_id'] = o1
            elif ty: e['samp'] = next((slot(x, SAMP_BASE, sb) for x in words if x * 4 >= SAMP_BASE and slot(x, SAMP_BASE, sb) is not None), None)
            else: e['samp_tex'] = True   # the sampler ID is in the same handle as the texture
            if t is not None and e['kind'] == 'tex' and t in set(res.get('fetch1d', ())): e['fetch1d'] = True
            return e
        if 3 <= tb <= 16 and ty in (0, 1, 3) and not (ty == 1 and not 3 <= sb <= 16):   # bindless: the handle is in the game's own uniform buffer
            e.update(kind=('img_ubo' if kind == 'i' else 'tbuf_ubo' if b['type'] == 'tbuf' else 'tex_ubo'), reg=tb - 3, off=o0 * 4)
            if ty == 3: e['samp_id'] = o1
            elif ty == 1: e['samp_ubo'] = [sb - 3, o1 * 4]
            # One combined handle: the sampler ID is in the same handle's high bits (gfx_vk_exec.c samp_for RS_TEX). Without a source the host bound its dummy sampler: DARK SOULS:
            # REMASTERED's scene shaders (SDK 4 GLSLC, fp_t_cb13_<word>) read every texture this way, and a pass trace showed "sampler 0: not registered (dummy)" for each of
            # them while the handles named samplers 0x101-0x105 (mirrored-repeat and clamp-to-edge ones)
            elif e['kind'] == 'tex_ubo': e['samp_tex'] = True
            return e
    e.update(kind='?'); return e

# Binary reflection (dm/android/gfx_vk.h dmsv_refl / dmsv_bind): header {u32 nbind, u32 c1_len, u32 local[3]}, nbind × 20-byte bindings, then c1.
# binding: u8 kind, u8 desc (descriptor type), u8 dim (SPIR-V Dim, 0xff none), u8 flags (1 arrayed, 2 multisampled, 4 written), u16 binding, u16 count,
#          i16 reg, u8 samp (sampler source), u8 fmt (SPIR-V image format), i32 off, i32 samp_arg
KIND = {'support': 0, 'cbuf': 1, 'c1': 2, 'drvcb': 3, 'sbuf': 4, 'ubo_ptr': 5, 'sbuf_ubo': 6, 'tex': 7, 'tbuf': 8, 'img': 9, 'img_tbuf': 10, 'tex_c2a': 11,
        'tex_ubo': 12, 'tbuf_ubo': 13, 'img_ubo': 14, 'texd': 15, 'sampd': 16, '?': 255}
DESC = {'ubo': 0, 'ssbo': 1, 'sampled': 2, 'storage': 3, 'tbuf': 4, 'storage_tbuf': 5, 'image': 6, 'sampler': 7, '?': 255}
SAMP_NONE, SAMP_TEX, SAMP_SLOT, SAMP_ID, SAMP_UBO = 0, 1, 2, 3, 4
kinds_all = collections.Counter()
def refl_bin(binds, c1, local):
    out = [struct.pack('<5I', len(binds), len(c1), *(list(local) + [1, 1, 1])[:3])]
    for e in binds:
        kind = KIND.get(e['kind'], 255)
        reg = e.get('reg'); reg = -1 if reg is None else reg
        if e['kind'] == 'drvcb': reg = e.get('bank', 0)
        off = e.get('off') if e.get('off') is not None else -1
        samp, sarg = SAMP_NONE, 0
        if e.get('samp_tex'): samp = SAMP_TEX
        elif e.get('samp') is not None: samp, sarg = SAMP_SLOT, e['samp']
        elif e.get('samp_id') is not None: samp, sarg = SAMP_ID, e['samp_id']
        elif e.get('samp_ubo'): samp, sarg = SAMP_UBO, e['samp_ubo'][0] | e['samp_ubo'][1] << 8
        flags = (1 if e.get('arrayed') else 0) | (2 if e.get('ms') else 0) | (4 if e.get('rw') else 0) | (8 if e.get('fetch1d') else 0)
        out.append(struct.pack('<BBBBHHhBBii', kind, DESC.get(e['type'], 255), 0xff if e.get('dim') is None else e['dim'], flags, e['binding'], e['count'],
                               reg, samp, e.get('fmt') or 0, off, sarg))
    return b''.join(out) + bytes(c1)

def remap_handles(spv, source, target, expected):
    """Remap a packed handle at stores feeding both texture and sampler array indices."""
    words = list(struct.unpack(f'<{len(spv) // 4}I', spv))
    constants, loads, uses, types = {}, {}, collections.defaultdict(set), {}
    bool_type = None
    instructions = []
    i = 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xffff
        if not count or i + count > len(words):
            raise ValueError('invalid SPIR-V instruction')
        ins = words[i:i + count]
        instructions.append((op, ins))
        if op == 20:  # OpTypeBool
            bool_type = ins[1]
        elif op == 21 and ins[2] == 32:  # OpTypeInt
            types[ins[1]] = ins[3]
        elif op == 43 and count == 4:  # OpConstant
            constants[ins[2]] = ins[3]
        elif op == 61:  # OpLoad
            loads[ins[2]] = (ins[3], ins[1])
        elif op in (194, 199) and ins[3] in loads:  # ShiftRightLogical / BitwiseAnd
            uses[loads[ins[3]]].add((op, constants.get(ins[4])))
        i += count

    handles = {ptr: typ for (ptr, typ), ops in uses.items()
               if typ in types and {(194, 20), (199, 0xfffff)} <= ops}
    if not bool_type or not expected or len(handles) != expected or len(set(handles.values())) != 1:
        raise ValueError('packed handle shader interface changed')
    typ = next(iter(handles.values()))
    raw, slot = words[3], words[3] + 1
    out = words[:5]
    out[3] += 2
    inserted = False
    stores = []
    for op, ins in instructions:
        if op == 54 and not inserted:  # constants must precede OpFunction
            out.extend([4 << 16 | 43, typ, raw, source, 4 << 16 | 43, typ, slot, target])
            inserted = True
        if op == 62 and ins[1] in handles:  # OpStore
            stores.append(ins[1])
            cond, value = out[3], out[3] + 1
            out[3] += 2
            out.extend([5 << 16 | 170, bool_type, cond, ins[2], raw])  # OpIEqual
            out.extend([6 << 16 | 169, typ, value, cond, slot, ins[2]])  # OpSelect
            ins[2] = value
        out.extend(ins)
    if sorted(stores) != sorted(handles):
        raise ValueError('expected one assignment per packed handle')
    return struct.pack(f'<{len(out)}I', *out)

def int_inputs(spv):
    """A vertex shader's inputs that it uses only as integers → {location: 'sint' / 'uint'} (dmsh_build.py int_inputs, the Mac chain's, from SPIR-V instead of MSL): every load of the input
    (whole, or one component through an access chain) is used only by OpBitcast. Maxwell does no type conversion reading vertex attribute registers: an integer format's bit pattern
    reaches the register and the shader bit-casts it. The answer table has no attribute types, so the translator declared every input float, the pipeline fed the integer format
    into a float input (undefined in Vulkan) before the bit casts: Dark Souls Remastered's scene vertex shaders read their RGBA16I attribute (loc 5) only through bit casts.
    Declared an integer (signed when a cast is to a signed type), the pipeline takes the integer format of the same layout (gfx_vk_exec.c vertex_format)"""
    w = struct.unpack(f'<{len(spv) // 4}I', spv); i = 5; loc, inputs, chain, loads, uses, other = {}, set(), {}, {}, collections.defaultdict(int), set()
    ins, signed, sint = [], {}, set()
    while i < len(w):
        n = w[i] >> 16; op = w[i] & 0xffff
        if n == 0: break
        ins.append((op, w[i + 1:i + n])); i += n
    for op, a in ins:
        if op == OP_DECORATE and len(a) > 2 and a[1] == 30: loc[a[0]] = a[2]   # Location
        elif op == OP_VARIABLE and a[2] == 1: inputs.add(a[1])                 # Input storage class
        elif op == 21: signed[a[0]] = bool(a[2])                               # OpTypeInt width signedness
        elif op == 23 and a[1] in signed: signed[a[0]] = signed[a[1]]          # OpTypeVector of an int
    for op, a in ins:
        if op == 65 and a[2] in inputs: chain[a[1]] = a[2]                     # OpAccessChain into an input
    for op, a in ins:
        if op == 61 and (a[2] in inputs or a[2] in chain): loads[a[1]] = a[2] if a[2] in inputs else chain[a[2]]   # OpLoad
    for op, a in ins:
        if op in (5, 6, 61, 65, 71, 72): continue   # names, decorations, the loads and chains themselves
        ops = a[2:] if op == 124 else a   # OpBitcast: result type, result, operand
        for x in ops:
            if x not in loads: continue
            if op == 124:
                uses[loads[x]] += 1
                if signed.get(a[0]): sint.add(loads[x])   # cast to a signed type: declared int, so a SINT format's fetch matches the input's numeric type
            else: other.add(loads[x])
    return {str(loc[v]): 'sint' if v in sint else 'uint' for v in uses if v not in other and v in loc}

def main():
    handle_remaps = {}
    table = GAME.table('android_shader_handles.tsv')
    if table.exists():
        for line in table.read_text().splitlines():
            if line and not line.startswith('#'):
                key, source, target, sites = line.split()
                handle_remaps[key] = int(source, 0), int(target, 0), int(sites)
    pc, loc = {}, {}
    for ln in Path(MAP).read_text(encoding='utf-8').splitlines():
        c = ln.rstrip('\n').split('\t')
        if len(c) > 4 and c[0].endswith('.bin'): loc.setdefault(c[3], (c[0], int(c[1])))
    cap = {k: v for k, v in captured(a.capture).items() if k not in loc}
    if cap: print(f'{len(cap)} shaders captured at run time: {" ".join(f"{k} ({st})" for k, (st, _) in sorted(cap.items()))}', flush=True)
    keys = sorted(k for k in (a.keys.split(',') if a.keys else [*loc, *cap]) if k in loc or k in cap)
    packs = {}
    w = tempfile.mkdtemp(prefix='shvk_', dir=os.environ.get('WORK'))
    jobs, c1s, stages = {}, {}, {}
    for k in keys:
        if k in cap: stages[k], rec = cap[k]
        else:
            name, idx = loc[k]
            if name not in packs: packs[name] = records((Path(a.pkg) / name).read_bytes())
            stages[k], rec = name[:2], packs[name][idx]
        jobs[k], c1s[k] = job_for(k, stages[k], rec, w)
    # A geometry shader's input primitive is the draw's (Maxwell's shader header doesn't say it; Ryujinx asks the GPU state): one translation for each kind, as the Mac
    # chain's _g0.._g4 (dmsh_build.py), triangles under the shader's own key and the others under gs_variant_key, chosen by the pipeline's topology (gfx_vk_shader.c
    # vk_shader_gs_topo). Xenoblade Chronicles 3's particle updates draw points into theirs: read as triangles, each took three particles as one and their records came out wrong
    gs_var = {}
    for k in [k for k in keys if stages[k] == 'gs']:
        for t in (0, 1, 2, 4):
            vk_ = f'{k}_t{t}'; jobs[vk_] = {**jobs[k], 'key': vk_, 'gs_input': t}; c1s[vk_] = c1s[k]; stages[vk_] = 'gs'; gs_var[vk_] = gs_variant_key(int(k, 16), t)
    keys = keys + sorted(gs_var)
    if not keys: raise ValueError('no shaders selected for export')
    if a.report and os.path.abspath(a.report) == os.path.abspath(a.out): raise ValueError('shader report must not overwrite the bundle')
    batch_errors, failures, rs = {}, {}, []
    def xl(ch):
        result = None
        try:
            jp = os.path.join(w, f'job_{abs(hash(ch[0]))}.json'); Path(jp).write_text(json.dumps([jobs[k] for k in ch]), encoding='utf-8')
            result = subprocess.run([a.dotnet, XLATE_DLL, jp, w], capture_output=True, text=True, errors='replace')
            shader_products.check_batch(result, len(ch))
            return result, None
        except Exception as e:
            return result, str(e)
    def translate(selected):
        chunks = [selected[i::a.jobs] for i in range(a.jobs) if selected[i::a.jobs]]
        with cf.ThreadPoolExecutor(len(chunks)) as ex:
            for ch, (result, error) in zip(chunks, ex.map(xl, chunks)):
                if result is not None: rs.append(result)
                # A failed process may have written valid products before it stopped.
                # Keep its diagnostic for keys with no output, and validate each product.
                batch_errors.update(dict.fromkeys(ch, error))
    print(f'translating {len(keys)} shaders in {min(a.jobs, len(keys))} batches ...', flush=True)
    translate(keys)
    # second pass (as the Mac chain's dmsh_build.py finish): vertex shaders with inputs used only as integers, translated again with those inputs reported as uint (int_inputs)
    again = []
    for k in keys:
        if stages[k] != 'vs' or k in failures: continue
        try:
            product = shader_products.output(w, k, STAGES[stages[k]])
            if product is None: continue
            extra = {l: t for l, t in int_inputs(product[0]).items() if l not in jobs[k]['attr']}
            if extra:
                jobs[k]['attr'].update(extra)
                # A failed retry must not retain the first pass's incompatible input interface.
                for suffix in ('.spv', '.res.json'): os.unlink(os.path.join(w, k + suffix))
                again.append(k)
        except Exception as e: failures[k] = str(e)
    if again:
        print(f'translating {len(again)} vertex shaders again with their integer inputs ...', flush=True)
        translate(again)
    # Standalone software GS passthrough needs only locations consumed by the bundle's fragment
    # shaders. Forwarding 16 vec4s plus built-ins exceeds a 64-component geometry input limit.
    # Guest reads used for culling remain tracked independently. XFB keeps the conservative path.
    fragment_inputs = 0
    for k in keys:
        if stages[k] != 'ps' or k in failures: continue
        try:
            product = shader_products.output(w, k, STAGES[stages[k]])
            if product is not None: fragment_inputs |= spirv_bindings(product[0])[2][1][0]
        except Exception as e: failures[k] = str(e)
    geometry = [k for k in keys if stages[k] == 'gs' and not jobs[k].get('xfb') and k not in failures]
    for k in geometry:
        jobs[k]['gs_passthrough_mask'] = fragment_inputs & 0xffff
        for suffix in ('.spv', '.res.json'):
            if os.path.exists(os.path.join(w, k + suffix)): os.unlink(os.path.join(w, k + suffix))
    if geometry:
        print(f'translating {len(geometry)} geometry shaders with fragment input mask {fragment_inputs:#x} ...', flush=True)
        translate(geometry)
    errs = collections.Counter()
    for r in rs:
        for ln in r.stderr.splitlines():
            if ln.strip(): errs[ln.strip()[:120]] += 1
    entries, fails, failed = [], collections.Counter(), []
    expected = [[f'{gs_var[k] if k in gs_var else int(k, 16):016x}', STAGES[stages[k]]] for k in keys]
    if len({k for k, _ in expected}) != len(expected): raise ValueError('shader keys collide with geometry variants')
    for k in keys:
        try:
            if k in failures: raise ValueError(failures[k])
            product = shader_products.output(w, k, STAGES[stages[k]])
            if product is None: raise ValueError(batch_errors[k] or 'no output')
            spv, res = product
            binds, local, io = spirv_bindings(spv)
            shader_products.validate_bindings(binds, res)
            if k in handle_remaps:
                spv = remap_handles(spv, *handle_remaps[k])
            st = stages[k]
            cl = [classify(st, b, res) for b in binds]
            for e in cl: kinds_all[e['kind']] += 1
            # header words 2-4: a compute shader's local size; a vertex shader's input locations (all, signed int, unsigned int); a pixel shader's output locations
            hdr3 = (local or jobs[k].get('local') or [1, 1, 1]) if st == 'cs' else io[1] if st == 'vs' else io[3] if st == 'ps' else [0, 0, 0]
            entries.append((gs_var[k] if k in gs_var else int(k, 16), STAGES.get(st, 0), spv, refl_bin(cl, c1s[k], hdr3)))
        except Exception as e:
            reason = str(e)
            failed.append([f'{gs_var[k] if k in gs_var else int(k, 16):016x}', reason])
            fails[reason] += 1
            print(f'[RED] {failed[-1][0]}: {reason}', flush=True)
    if a.zstd: print(f'compressing {len(entries)} shaders (zstd level {a.zstd}, {a.jobs} threads) ...', flush=True)
    ver, raw, stored = dmsv.write(a.out, entries, level=a.zstd, jobs=a.jobs)
    if a.report:
        with open(a.report, 'w', encoding='utf-8') as f: json.dump(dict(schema=1, expected=expected, failed=failed), f)
    print(f'{len(entries)} of {len(keys)} shaders → {a.out} ({os.path.getsize(a.out) / 2**20:.1f} MB, version {ver}: SPIR-V {raw / 2**20:.1f} MB'
          f'{f", {stored / 2**20:.1f} MB compressed" if ver > 1 else ""}); failed: {dict(fails)}; binding kinds: {dict(kinds_all)}')
    for e, n in errs.most_common(8): print(f'  translator: {n:5d}  {e}')

main()
