# Changes to a translated shader's SPIR-V that a game profile asks for (shader_vk.py applies them before it reads the module's bindings):
#   demote_discards: game.tsv "android_discard demote", every pixel shader
#   text_patch: SPIRV = {shader key: function} of the profile's patches.py, one shader, on its spirv-dis text
import os, struct, subprocess

OP_EXTENSION, OP_CAPABILITY, OP_LABEL, OP_SELMERGE, OP_BRANCH, OP_BRCOND, OP_SWITCH, OP_KILL, OP_DEMOTE, CAP_DEMOTE = 10, 17, 248, 247, 249, 250, 251, 252, 5380, 5379
EXT_DEMOTE = b'SPV_EXT_demote_to_helper_invocation\0'; EXT_DEMOTE += b'\0' * (-len(EXT_DEMOTE) % 4)

# A pixel shader's discards as demotions to a helper invocation.
# The translator writes a discard as OpKill. On the Switch a discarded pixel still takes part in its 2x2 quad's derivatives; after an OpKill the quad's other pixels
# have none on an Adreno, and a texture they read afterwards comes from its smallest level. Trails in the Sky 1st Chapter's materials discard for their dithered fades
# (level-of-detail cross-fades, objects near the camera) before they sample: a leaf texture's smallest level is almost transparent, the alpha test that follows
# discards the pixel too, and a tree's crown was gone for the 20 frames of every cross-fade (AYN Odin 3, Turnip, the fade held at 50%: the crown came back with
# either discard disabled, never with both). OpDemoteToHelperInvocation (SPV_EXT_demote_to_helper_invocation, core in Vulkan 1.3, enabled by gfx_vk_core.c where
# the device has it) is the Switch's behaviour: the pixel writes nothing and goes on running for its neighbours. Only an OpKill that is the whole end of a
# selection's arm is changed (the arm then branches to the selection's merge block); any other is left and counted.
def demote_discards(spv):
    """(the module, discards changed, discards left as they were); the module is None when nothing changed"""
    w = list(struct.unpack('<%dI' % (len(spv) // 4), spv)); i = 5; ins = []; arm = {}; label = None; merge = None; last_cap = None; has = False; left = 0
    while i < len(w):
        op, n = w[i] & 0xffff, w[i] >> 16
        if n == 0: return None, 0, 0
        if op == OP_CAPABILITY: last_cap = i + n; has |= w[i + 1] == CAP_DEMOTE
        elif op == OP_LABEL: label = w[i + 1]
        elif op == OP_SELMERGE: merge = w[i + 1]
        elif op == OP_BRCOND:
            if merge is not None: arm[w[i + 2]] = merge; arm[w[i + 3]] = merge
            merge = None
        elif op in (OP_BRANCH, OP_SWITCH): merge = None
        elif op == OP_KILL:
            if label in arm and arm[label] != label: ins.append((i, arm[label]))
            else: left += 1
        i += n
    if not ins or last_cap is None: return None, 0, left
    o = w[:last_cap] + ([] if has else [2 << 16 | OP_CAPABILITY, CAP_DEMOTE, (1 + len(EXT_DEMOTE) // 4) << 16 | OP_EXTENSION] + list(struct.unpack('<%dI' % (len(EXT_DEMOTE) // 4), EXT_DEMOTE))); p = last_cap
    for at, m in ins: o += w[p:at] + [1 << 16 | OP_DEMOTE, 2 << 16 | OP_BRANCH, m]; p = at + 1
    o += w[p:]
    return struct.pack('<%dI' % len(o), *o), len(ins), left

def text_patch(spv, fn, tools, work, name):
    """One shader through its profile's patch: spirv-dis, fn(the text's lines) -> lines, spirv-as with the same ids, spirv-val. tools is the folder of the three
    programs (the NDK's shader-tools). Raises ValueError with the reason when the patch does not take: fn refuses a text that is not the one it was written for
    (the translator changed), the tools are missing, or the result does not assemble or validate."""
    exe = '.exe' if os.name == 'nt' else ''; prog = {t: os.path.join(tools or '', f'spirv-{t}{exe}') for t in ('dis', 'as', 'val')}
    if not tools or not all(os.path.isfile(p) for p in prog.values()): raise ValueError(f'no spirv-dis, spirv-as and spirv-val in {tools or "(no --spirv-tools)"}')
    a, t, b = (os.path.join(work, f'{name}.patch.{x}') for x in ('in.spv', 'txt', 'out.spv'))
    def run(cmd):
        r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
        if r.returncode: raise ValueError(f'{os.path.basename(cmd[0])}: {(r.stderr or r.stdout).strip()[:200]}')
        return r.stdout
    with open(a, 'wb') as f: f.write(spv)
    lines = fn(run([prog['dis'], a]).split('\n'))
    with open(t, 'w', encoding='utf-8') as f: f.write('\n'.join(lines))
    run([prog['as'], '--target-env', 'spv1.3', '--preserve-numeric-ids', t, '-o', b]); run([prog['val'], '--target-env', 'spv1.3', b])
    with open(b, 'rb') as f: return f.read()
