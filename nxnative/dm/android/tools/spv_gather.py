"""Remove unused ImageGatherExtended, ported from nxnative prepare_spirv.

Source: b9e3c2b66, preserved at 1f01e28c87da5233dfa14fb31fabf1535df33013.
Other capability cleanup already happens in the Android native loader.
"""
import struct
from spv_module import decode, opcode


def lower(data):
    header, ins = decode(data)
    # The shared generator declares ImageGatherExtended unconditionally. The
    # SPIR-V grammar requires it for ImageOperands Offset/ConstOffsets, not for
    # ordinary gather or ConstOffset. Maxwell offsets often become coordinate
    # arithmetic instead, so keeping this declaration rejects capable devices.
    # Values are the word index of ImageOperands (including the opcode word).
    masks = {op: 5 for op in (87, 88, 91, 92, 95, 98, 305, 306, 309, 310, 313, 320)}
    masks.update({op: 6 for op in (89, 90, 93, 94, 96, 97, 307, 308, 311, 312, 314, 315)})
    masks.update({99: 4, 4545: 7, 5283: 7})  # write, QCOM gather, NV footprint
    if not any(len(x) > masks[opcode(x)] and x[masks[opcode(x)]] & 0x30
               for x in ins if opcode(x) in masks):
        words = list(header)
        for x in ins:
            if opcode(x) != 17 or x[1] != 25:
                words.extend(x)
        return struct.pack(f'<{len(words)}I', *words)
    return data
