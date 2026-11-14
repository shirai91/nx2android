"""The Vulkan export requires extended gather only when its operands need it."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'nxnative/dm/android/tools'))
from spv_module import decode, opcode
from spv_gather import lower


def capabilities(data):
    return {x[1] for x in decode(data)[1] if opcode(x) == 17}

GATHER = '''OpCapability Shader
OpCapability Float64
OpCapability ImageGatherExtended
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %out
OpExecutionMode %main OriginUpperLeft
OpDecorate %tex DescriptorSet 0
OpDecorate %tex Binding 0
OpDecorate %out Location 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%int = OpTypeInt 32 1
%int2 = OpTypeVector %int 2
%float2 = OpTypeVector %float 2
%float4 = OpTypeVector %float 4
%zero = OpConstant %int 0
%four = OpConstant %int 4
%half = OpConstant %float 0.5
%offset = OpConstantNull %int2
%offsets_type = OpTypeArray %int2 %four
%offsets = OpConstantComposite %offsets_type %offset %offset %offset %offset
%coord = OpConstantComposite %float2 %half %half
%image = OpTypeImage %float 2D DEPTH 0 0 1 Unknown
%sampled = OpTypeSampledImage %image
%ptr = OpTypePointer UniformConstant %sampled
%tex = OpVariable %ptr UniformConstant
%outptr = OpTypePointer Output %float4
%out = OpVariable %outptr Output
%main = OpFunction %void None %fn
%label = OpLabel
%loaded = OpLoad %sampled %tex
%color = OP %float4 %loaded %coord COMPONENT OPERANDS
OpStore %out %color
OpReturn
OpFunctionEnd
'''


@unittest.skipUnless(shutil.which('spirv-as') and shutil.which('spirv-val'), 'requires SPIRV-Tools')
class GatherCapabilityTests(unittest.TestCase):
    def test_drop_unused_but_retain_offset_requirements(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'shader.spv'
            for shadow in (False, True):
                for operands, needed in (('', False), ('None', False), ('ConstOffset %offset', False),
                                         ('Offset %offset', True), ('ConstOffsets %offsets', True)):
                    with self.subTest(shadow=shadow, operands=operands):
                        source = GATHER.replace('DEPTH', str(int(shadow))).replace('COMPONENT', '%half' if shadow else '%zero')
                        source = source.replace('OPERANDS', operands).replace('OP', 'OpImageDrefGather' if shadow else 'OpImageGather')
                        subprocess.run(['spirv-as', '--target-env', 'vulkan1.1', '-o', path, '-'], input=source, text=True, check=True)
                        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', path], check=True)
                        original = path.read_bytes()
                        data = lower(original)
                        before, after = decode(original), decode(data)
                        self.assertEqual(after[0], before[0])
                        self.assertEqual(after[1], [x for x in before[1] if needed or opcode(x) != 17 or x[1] != 25])
                        self.assertEqual(lower(data), data)
                        self.assertEqual(25 in capabilities(data), needed)
                        path.write_bytes(data)
                        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', path], check=True)


if __name__ == '__main__':
    unittest.main()
