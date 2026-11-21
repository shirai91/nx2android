"""Donor offset acceptance cases with equivalent bounded SPIR-V fixtures."""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'nxnative/dm/android/tools'))
import spv_module as vk, spv_texture_offsets as offsets
from spv_builder import instruction

ASSEMBLY = '''OpCapability Shader
OpCapability ImageGatherExtended
%glsl = OpExtInstImport "GLSL.std.450"
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %tex DescriptorSet 0
OpDecorate %tex Binding 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%int = OpTypeInt 32 1
%int2 = OpTypeVector %int 2
%float2 = OpTypeVector %float 2
%float4 = OpTypeVector %float 4
%zero = OpConstant %int 0
%one = OpConstant %int 1
%six = OpConstant %int 6
%fzero = OpConstant %float 0
%fhalf = OpConstant %float 0.5
%fone = OpConstant %float 1
%coord = OpConstantComposite %float2 %fhalf %fhalf
%image = OpTypeImage %float 2D 0 0 0 1 Unknown
%sampled = OpTypeSampledImage %image
%ptr = OpTypePointer UniformConstant %sampled
%pf = OpTypePointer Function %int
%tex = OpVariable %ptr UniformConstant
%main = OpFunction %void None %fn
%label = OpLabel
%loaded = OpLoad %sampled %tex
%bounded = OpBitFieldSExtract %int %six %zero %one
%offset = OpCompositeConstruct %int2 %bounded %bounded
%lod = OpExtInst %float %glsl FClamp %fhalf %fzero %fone
%color = OpImageSampleExplicitLod %float4 %loaded %coord Lod|Offset %lod %offset
OpReturn
OpFunctionEnd
'''


def encode(header, code):
    words = list(header)
    for x in code: words.extend(x)
    return struct.pack(f'<{len(words)}I', *words)


@unittest.skipUnless(all(shutil.which(x) for x in ('spirv-as', 'spirv-val')), 'requires NDK SPIR-V tools on PATH')
class ExactOffsetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='exact-offset-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.work = Path(cls.temporary.name)
        source=ASSEMBLY.replace('%one = OpConstant %int 1', '%one = OpConstant %int 4\n%bool = OpTypeBool')
        source=source.replace('%main = OpFunction', '%unknown = OpUndef %int\n%main = OpFunction').replace('OpBitFieldSExtract %int %six', 'OpBitFieldSExtract %int %unknown')
        local=source.replace('%label = OpLabel','%label = OpLabel\n%local = OpVariable %pf Function')
        local=local.replace('%offset = OpCompositeConstruct %int2 %bounded %bounded','OpStore %local %bounded\n%read = OpLoad %int %local\n%offset = OpCompositeConstruct %int2 %read %read')
        variants={0:source.replace('Lod|Offset %lod %offset','Lod %lod'),1:source,3:local,9:source.replace('Lod|Offset %lod %offset','Grad|Offset %coord %coord %offset')}
        cls.inputs={}
        for variant,assembly in variants.items():
            cls.inputs[variant]=subprocess.check_output(['spirv-as','--target-env','spv1.3','-o','-','-'],input=assembly.encode())

    def validate(self, raw):
        lowered = offsets.lower(raw)
        result = lowered
        path = self.work / 'export.spv'; path.write_bytes(result)
        run = subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', path], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        return vk.decode(result)[1]

    def assemble(self, source):
        path = self.work / 'source.spv'
        subprocess.run(['spirv-as', '--target-env', 'spv1.3', '-o', path, '-'], input=source, text=True, check=True)
        subprocess.run(['spirv-val', '--target-env', 'spv1.3', path], check=True)
        return path.read_bytes()

    def test_extinst_produced_lod_is_preserved(self):
        result = self.validate(self.assemble(ASSEMBLY))
        self.assertTrue(any(vk.opcode(x) == 12 for x in result))
        self.assertEqual(sum(vk.opcode(x) == 88 for x in result), 4)

    def test_separate_texture_and_sampler_combine_inside_each_case(self):
        source = ASSEMBLY.replace('OpDecorate %tex Binding 0', 'OpDecorate %tex Binding 0\nOpDecorate %samp DescriptorSet 0\nOpDecorate %samp Binding 1')
        source = source.replace('%ptr = OpTypePointer UniformConstant %sampled', '''%ptr = OpTypePointer UniformConstant %image
%sampler = OpTypeSampler
%ps = OpTypePointer UniformConstant %sampler
%samp = OpVariable %ps UniformConstant''')
        source = source.replace('%loaded = OpLoad %sampled %tex', '''%im = OpLoad %image %tex
%sa = OpLoad %sampler %samp
%loaded = OpSampledImage %sampled %im %sa''')
        self.validate(self.assemble(source))

    def test_initialized_local_cannot_be_proven_from_only_its_store(self):
        source = ASSEMBLY.replace('%label = OpLabel', '%label = OpLabel\n%local = OpVariable %pf Function %six')
        source = source.replace('%offset = OpCompositeConstruct %int2 %bounded %bounded', '''%read = OpLoad %int %local
OpStore %local %bounded
%offset = OpCompositeConstruct %int2 %read %read''')
        with self.assertRaisesRegex(ValueError, 'unproven texture sample offset range'):
            offsets.lower(self.assemble(source))

    def test_local_passed_to_writer_is_not_proven_from_direct_store(self):
        source = ASSEMBLY.replace('%pf = OpTypePointer Function %int', '%pf = OpTypePointer Function %int\n%writer_type = OpTypeFunction %void %pf')
        source = source.replace('%label = OpLabel', '%label = OpLabel\n%local = OpVariable %pf Function')
        source = source.replace('%offset = OpCompositeConstruct %int2 %bounded %bounded', 'OpStore %local %bounded\n%called = OpFunctionCall %void %writer %local\n%read = OpLoad %int %local\n%offset = OpCompositeConstruct %int2 %read %read')
        source += '%writer = OpFunction %void None %writer_type\n%param = OpFunctionParameter %pf\n%entry = OpLabel\nOpStore %param %six\nOpReturn\nOpFunctionEnd\n'
        with self.assertRaisesRegex(ValueError, 'escaped texture offset local'):
            offsets.lower(self.assemble(source))

    def test_literal_equal_to_local_pointer_id_is_not_an_escape(self):
        header, code = vk.decode(self.inputs[3])
        local = next(x for x in code if vk.opcode(x) == 59 and x[3] == 7)
        int_type = next(x[1] for x in code if vk.opcode(x) == 21 and x[2:] == (32, 1))
        constant = instruction(43, int_type, header[3], local[2])
        header = (*header[:3], header[3]+1, header[4])
        at = next(i for i, x in enumerate(code) if vk.opcode(x) == 54)
        self.validate(encode(header, code[:at] + [constant] + code[at:]))

    def test_untouched_shader_is_byte_identical(self):
        self.assertEqual(offsets.lower(self.inputs[0]), self.inputs[0])

    def test_all_signed_four_bit_tuples_preserve_original_lod_or_grad(self):
        for variant in (1, 3, 9):
            with self.subTest(variant=variant):
                code = self.validate(self.inputs[variant])
                samples = [x for x in code if vk.opcode(x) == 88]
                self.assertEqual(len(samples), 256)
                self.assertEqual({x[5] for x in samples}, {12 if variant == 9 else 10})  # Grad/Lod + ConstOffset
                # Sampler operations stay native: no size-query/displacement approximation.
                self.assertFalse(any(vk.opcode(x) in (103, 104, 105) for x in code))

    def test_implicit_samples_preserve_derivative_control_flow(self):
        header, code = vk.decode(self.inputs[1]); rewritten = []
        for x in code:
            if vk.opcode(x) == 15: x = instruction(15, 4, *x[2:])
            if vk.opcode(x) == 16 and x[2] == 17: x = instruction(16, x[1], 7)
            if vk.opcode(x) == 88: x = instruction(87, *x[1:5], x[5] & ~2, *x[7:])
            rewritten.append(x)
        result = self.validate(encode(header, rewritten))
        self.assertEqual(sum(vk.opcode(x) == 87 for x in result), 256)
        self.assertFalse(any(vk.opcode(x) in (250, 251) for x in result))

    def test_helper_does_not_break_caller_loop_structure(self):
        header, code = vk.decode(self.inputs[3]); bound = header[3]
        loop, body, continuation, done, false, boolean = range(bound, bound+6)
        header = (*header[:3], bound+6, header[4]); rewritten = []; inserted = False
        for x in code:
            op = vk.opcode(x)
            if op == 54:
                rewritten += [instruction(20, boolean), instruction(42, boolean, false)]
            if op == 248: inserted = True; rewritten.append(x); continue
            if inserted and op != 59:
                rewritten += [instruction(249, loop), instruction(248, loop), instruction(246, done, continuation, 0),
                              instruction(249, body), instruction(248, body)]
                inserted = False
            if op == 253:
                rewritten += [instruction(249, continuation), instruction(248, continuation),
                              instruction(250, false, loop, done), instruction(248, done)]
            rewritten.append(x)
        # The source already declares bool; use it instead of a duplicate type.
        original_bool = next(x[1] for x in code if vk.opcode(x) == 20)
        rewritten = [instruction(42, original_bool, false) if vk.opcode(x) == 42 and x[2] == false else x
                     for x in rewritten if not (vk.opcode(x) == 20 and x[1] == boolean)]
        result = self.validate(encode(header, rewritten))
        self.assertTrue(any(vk.opcode(x) == 246 and x[1:3] == (done, continuation) for x in result))

    def test_unproven_or_wider_offset_ranges_are_rejected(self):
        header, code = vk.decode(self.inputs[1])
        for mode in ('unbounded', 'wide'):
            rewritten = []
            widths = {x[5] for x in code if vk.opcode(x) == 202}
            for x in code:
                if mode == 'unbounded' and vk.opcode(x) == 202: x = instruction(83, *x[1:4])
                if mode == 'wide' and vk.opcode(x) == 43 and x[2] in widths: x = instruction(43, *x[1:3], 8)
                rewritten.append(x)
            with self.subTest(mode=mode), self.assertRaisesRegex(ValueError, 'texture.*offset'):
                offsets.lower(encode(header, rewritten))


if __name__ == '__main__':
    unittest.main()
