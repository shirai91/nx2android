"""Required Module methods copied from the Android parity SPIR-V builder.

Source: nxnative 1f01e28c, dm/android/tools/spv_capture.py.
Geometry/output-capture methods are deliberately not part of this port.
"""
import collections
from spv_module import decode, opcode


def instruction(op, *args):
    return ((len(args) + 1) << 16 | op, *args)


class Module:
    def __init__(self, data):
        self.header, self.code = decode(data)
        self.next_id = self.header[3]
        self.declarations = []
        self.annotations = []
        self.names = []
        self.interfaces = []
        self.types = {i[1]: i for i in self.code if 19 <= opcode(i) <= 39}
        self.constants = {i[2]: i[3] for i in self.code if opcode(i) == 43 and len(i) == 4}
        self.decorations = collections.defaultdict(dict)
        self.members = collections.defaultdict(dict)
        for i in self.code:
            if opcode(i) == 71: self.decorations[i[1]][i[2]] = i[3:]
            if opcode(i) == 72: self.members[i[1], i[2]][i[3]] = i[4:]
        self.uint = self.ty(21, 32, 0)
        self.boolean = self.ty(20)

    def ident(self):
        result = self.next_id; self.next_id += 1
        return result

    def ty(self, op, *args):
        for key, value in self.types.items():
            if opcode(value) == op and value[2:] == args: return key
        key = self.ident(); value = instruction(op, key, *args)
        self.types[key] = value; self.declarations.append(value)
        return key

    def const(self, value, ty=None):
        ty = self.uint if ty is None else ty
        for i in self.code + self.declarations:
            if opcode(i) == 43 and i[1] == ty and i[3:] == (value,): return i[2]
        key = self.ident(); self.declarations.append(instruction(43, ty, key, value))
        self.constants[key] = value
        return key

    def value(self, code, op, ty, *args):
        key = self.ident(); code.append(instruction(op, ty, key, *args)); return key

    def decorate(self, key, decoration, *args):
        self.annotations.append(instruction(71, key, decoration, *args))
