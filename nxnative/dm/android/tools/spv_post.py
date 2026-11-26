"""Changes to a translated shader's SPIR-V that a game profile asks for (shader_vk.py applies them before it reads the module's bindings):
  demote_discards: game.tsv "android_discard demote", every pixel shader
  text_patch: SPIRV = {shader key: function} of the profile's patches.py, one shader

Two ways to patch a module:
  * a text patch (the profile's function takes the lines of the shader's spirv-dis text and returns the lines): it needs spirv-dis, spirv-as and spirv-val
    (the NDK's shader-tools). Without them the patch cannot run and the shader stays as it was translated.
  * a binary patch (the profile's function takes a Module, this file's reader of the SPIR-V words, and returns the module): the same patch without any external tool.
    The profile marks it with @spv_post.binary. The patch raises ValueError when the module is not the one it was written for; the shader then stays as it was
    translated, with a [RED] line (shader_vk.py).
"""
import os, struct, subprocess

from spv_builder import instruction as spv_instruction   # the encoder (the word count in the high 16 bits)
from spv_module import decode as spv_decode, opcode as spv_opcode   # the decoder (the header, the instructions)

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


# ---- The SPIR-V words: a reader and a writer for the instructions a patch needs, on the decoder and the encoder ----
# The module's instructions, each as its words (the opcode word first). A patch that works on the words needs no spirv-dis and no spirv-as, so it runs on a
# phone (the shader stage runs there) and on a build machine alike.
class Module:
    """A SPIR-V module as its instructions, read with the decoder (spv_module.decode) and written back with its encoder (spv_builder.instruction)."""
    def __init__(self, data):
        self.header, self.code = spv_decode(data)      # the header's words, the instructions' word lists
        self.header = list(self.header)
        self.code = [list(x) for x in self.code]       # the instructions as lists, so a patch can edit a word in place
    def op(self, i): return spv_opcode(self.code[i])
    def args(self, i): return list(self.code[i][1:])
    def find(self, op, pred=None):
        """the indexes of the instructions with the opcode, whose operands the predicate accepts"""
        return [i for i in range(len(self.code)) if self.op(i) == op and (pred is None or pred(self.args(i)))]
    def ident(self):
        """a fresh id (the module's bound grows with it)"""
        b = self.header[3]; self.header[3] = b + 1; return b
    def insert(self, i, ins): self.code[i:i] = [list(x) for x in ins]
    def replace(self, i, ins): self.code[i:i + 1] = [list(x) for x in ins]
    def bytes(self):
        out = list(self.header)
        for ins in self.code: out.extend(ins)
        data = struct.pack('<%dI' % len(out), *out)
        # the self-check: the module must decode again, with the instructions it was written with
        header, code = spv_decode(data)
        if len(code) != len(self.code): raise ValueError(f'the patched module decodes into {len(code)} instructions, {len(self.code)} expected')
        return data

def instruction(op, *args): return list(spv_instruction(op, *args))

# The profile marks a binary patch with this decorator, so that one function can be both a text patch and a binary patch.
def binary(fn):
    fn.spv_binary = True
    return fn

def binary_patch(spv, fn):
    """One shader through the profile's binary patch: the module, the patch (it changes the module and returns it, or returns the bytes), the module's bytes.
    A patch that raises ValueError (the module is not the one it was written for) leaves the shader as it was translated."""
    m = Module(spv)
    out = fn(m)
    if out is None: out = m
    if isinstance(out, Module): out = out.bytes()
    if not isinstance(out, (bytes, bytearray)) or len(out) % 4: raise ValueError('the patch did not return a SPIR-V module')
    return bytes(out)

def text_patch(spv, fn, tools, work, name):
    """One shader through its profile's patch. A binary patch (the profile marked it with @spv_post.binary) works on the SPIR-V words and needs no external
    tool. A text patch (the profile's function takes the lines of the shader's spirv-dis text) needs spirv-dis, spirv-as and spirv-val (the NDK's
    shader-tools): spirv-dis, fn(the text's lines) -> lines, spirv-as with the same ids, spirv-val. Raises ValueError with the reason when the patch does not
    take: fn refuses a text that is not the one it was written for (the translator changed), the tools are missing, or the result does not assemble or validate."""
    if getattr(fn, 'spv_binary', False): return binary_patch(spv, fn)
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
