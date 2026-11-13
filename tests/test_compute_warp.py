"""A workgroup barrier is only valid when all invocations reach the shuffle."""
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nxnative/dm/android/tools"))
import spv_warp
from spv_module import decode, opcode


def assembly(*, branch=False, existing_index=False, declared_index_only=False, specialize_size=False, shared=False, mask=None):
    extra = ' %index' if existing_index and not declared_index_only else ''
    annotations = 'OpDecorate %index BuiltIn LocalInvocationIndex\n' if existing_index else ''
    variables = '%index = OpVariable %input Input\n' if existing_index else ''
    capabilities = ''
    if mask:
        capabilities = 'OpCapability GroupNonUniformBallot\n'
        extra += ' %mask'
        annotations += 'OpDecorate %mask BuiltIn ' + mask + '\n'
        variables += '%vec = OpTypeVector %uint 4\n%mptr = OpTypePointer Input %vec\n%mask = OpVariable %mptr Input\n'
    if specialize_size:
        annotations += 'OpDecorate %size BuiltIn WorkgroupSize\nOpDecorate %count SpecId 0\n'
        variables += '%uvec3 = OpTypeVector %uint 3\n%count = OpSpecConstant %uint 64\n%size = OpSpecConstantComposite %uvec3 %count %one %one\n'
    if shared:
        variables += '%wg = OpTypePointer Workgroup %uint\n%scratch = OpVariable %wg Workgroup\n'
    before = '%b = OpIEqual %bool %id %one\nOpSelectionMerge %merge None\nOpBranchConditional %b %then %merge\n%then = OpLabel\n' if branch else ''
    after = 'OpBranch %merge\n%merge = OpLabel\n' if branch else ''
    return f"""OpCapability Shader
OpCapability GroupNonUniformShuffle
{capabilities}OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "long_entry_name" %lane{extra}
OpExecutionMode %main LocalSize 64 1 1
OpDecorate %lane BuiltIn SubgroupLocalInvocationId
{annotations}%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%bool = OpTypeBool
%one = OpConstant %uint 1
%scope = OpConstant %uint 3
%input = OpTypePointer Input %uint
%lane = OpVariable %input Input
{variables}%main = OpFunction %void None %fn
%entry = OpLabel
%id = OpLoad %uint %lane
{before}%value = OpGroupNonUniformShuffle %uint %scope %id %one
{after}OpReturn
OpFunctionEnd
"""


@unittest.skipUnless(shutil.which('spirv-as') and shutil.which('spirv-val'), 'requires SPIR-V tools')
class ComputeWarp(unittest.TestCase):
    def assemble(self, source):
        raw = subprocess.run(['spirv-as', '--target-env', 'vulkan1.1', '-', '-o', '-'],
                             input=source.encode(), capture_output=True, check=True).stdout
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=raw, check=True)
        return raw

    def module(self, **parameters):
        return self.assemble(assembly(**parameters))

    def test_vote_equal_requires_boolean_operand(self):
        source = assembly().replace('GroupNonUniformShuffle\n', 'GroupNonUniformVote\n')
        source = source.replace('OpGroupNonUniformShuffle %uint %scope %id %one', 'OpGroupNonUniformAllEqual %bool %scope %id')
        raw = self.assemble(source)
        self.assertEqual(spv_warp.lower(raw), raw)
        raw = self.assemble(source.replace('%value =', '%p = OpIEqual %bool %id %one\n%value =').replace('%scope %id\n', '%scope %p\n'))
        portable = spv_warp.lower(raw)
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=portable, check=True)
        ops = [opcode(i) for i in decode(portable)[1]]
        self.assertIn(241, ops)  # one 32-bit warp ballot
        self.assertNotIn(336, ops)

    def test_straight_line_uses_workgroup_exchange_and_one_linear_index(self):
        for existing_index, declared_only in ((False, False), (True, False), (True, True)):
            with self.subTest(existing_index=existing_index, declared_only=declared_only):
                result = spv_warp.lower(self.module(existing_index=existing_index, declared_index_only=declared_only))
                subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=result, check=True)
                _, code = decode(result)
                self.assertEqual(sum(opcode(i) == 224 for i in code), 2)
                self.assertFalse(any(opcode(i) == 345 for i in code))
                self.assertEqual(sum(opcode(i) == 71 and i[2:4] == (11, 29) for i in code), 1)
                self.assertEqual(spv_warp.lower(result), result)

    def test_divergence_and_specialized_group_sizes_keep_native_path(self):
        for option in ('branch', 'specialize_size'):
            with self.subTest(option=option):
                raw = self.module(**{option: True})
                self.assertEqual(spv_warp.lower(raw), raw)

    def test_workgroup_size_literal_must_match_each_dimension(self):
        source = assembly(specialize_size=True).replace('OpDecorate %count SpecId 0\n', '')
        for width in (64, 32):
            raw = self.assemble(source.replace('OpSpecConstant %uint 64', f'OpConstant %uint {width}')
                                .replace('OpSpecConstantComposite', 'OpConstantComposite'))
            with self.subTest(width=width):
                self.assertEqual(spv_warp.lower(raw) != raw, width == 64)

    @unittest.skipUnless(shutil.which('glslc') and shutil.which('spirv-opt'), 'requires GLSL compiler and optimizer')
    def test_predicated_shuffle_may_move_only_with_register_operations(self):
        shader = """#version 450
#extension GL_KHR_shader_subgroup_shuffle : require
layout(local_size_x=64) in;
layout(set=0,binding=0,std430) buffer Data { uint words[]; };
void main() {
    uint id = gl_LocalInvocationIndex;
    uint v = id + 9;
    if (id < 32) {
        BODY
    }
    words[id] = v;
}
"""
        for body, portable in (('v = subgroupShuffle(v, 31);', True),
                               ('v = subgroupShuffle(atomicAdd(words[id + 64], 1), 31);', False),
                               ('v = subgroupShuffle(words[id + 64], 31);', False)):
            raw = subprocess.run(['glslc', '--target-env=vulkan1.1', '-fshader-stage=comp', '-', '-o', '-'],
                                 input=shader.replace('BODY', body).encode(), capture_output=True, check=True).stdout
            with self.subTest(body=body):
                result = spv_warp.lower(raw, shutil.which('spirv-opt'))
                subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=result, check=True)
                ops = [opcode(inst) for inst in decode(result)[1]]
                self.assertEqual(345 not in ops and 224 in ops, portable)
                if not portable:
                    self.assertEqual(result, raw)

    @unittest.skipUnless(shutil.which('glslc') and shutil.which('spirv-opt'), 'requires GLSL compiler and optimizer')
    def test_sparse_votes_after_an_early_return_or_inside_a_branch(self):
        template = """#version 450
#extension GL_KHR_shader_subgroup_vote : require
layout(local_size_x=48) in;
layout(set=0,binding=0,std430) buffer Data { uint words[]; };
void main() {
    uint lane = gl_LocalInvocationIndex;
    bool activeLane = (lane & 4) == 0;
    bool value = false;
    BODY
    words[lane] = uint(value);
}
"""
        cases = [
            ('if (activeLane) { value = subgroupAllEqual(lane < 19); }', True),
            ('if (!activeLane) return; value = subgroupAny(lane < 19);', True),
            ('if (!activeLane) return; value = subgroupAny(words[lane] == 1);', False),
            ('if (activeLane) { value = subgroupAll(atomicAdd(words[lane], 1) == 0); }', False),
        ]
        for body, can_move in cases:
            with self.subTest(body=body):
                src = template.replace('BODY', body).encode()
                raw = subprocess.check_output(['glslc', '--target-env=vulkan1.1', '-fshader-stage=comp', '-', '-o', '-'], input=src)
                result = spv_warp.lower(raw, shutil.which('spirv-opt'))
                subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=result, check=True)
                ops = set(map(opcode, decode(result)[1]))
                self.assertEqual(224 in ops and ops.isdisjoint((334, 335, 336)), can_move)
                if not can_move:
                    self.assertEqual(result, raw)

    def test_collective_after_merge_or_before_return_is_converged(self):
        branch = assembly(branch=True)
        exchange = '%value = OpGroupNonUniformShuffle %uint %scope %id %one\n'
        for anchor in ('%merge = OpLabel\n', '%id = OpLoad %uint %lane\n'):
            source = branch.replace(exchange, '').replace(anchor, anchor + exchange)
            with self.subTest(anchor=anchor):
                raw = self.assemble(source)
                result = spv_warp.lower(raw)
                self.assertNotEqual(raw, result)
                subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=result, check=True)
        # An early return lets part of the group bypass the post-merge barrier.
        raw = self.assemble(branch.replace(exchange, '').replace('OpBranch %merge', 'OpReturn')
                            .replace('%merge = OpLabel\n', '%merge = OpLabel\n' + exchange))
        self.assertEqual(spv_warp.lower(raw), raw)

    def test_collective_helper_requires_a_converged_call_site(self):
        source = assembly(branch=True)
        exchange = '%value = OpGroupNonUniformShuffle %uint %scope %id %one'
        helper = '%helper = OpFunction %void None %fn\n%h = OpLabel\n' + exchange.replace('%id', '%one') + '\nOpReturn\nOpFunctionEnd\n'
        call = '%call = OpFunctionCall %void %helper'
        raw = self.assemble(source.replace(exchange, call) + helper)
        self.assertEqual(spv_warp.lower(raw), raw)
        raw = self.assemble(source.replace(exchange, '').replace('OpReturn', call + '\nOpReturn') + helper)
        result = spv_warp.lower(raw)
        self.assertNotEqual(result, raw)
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=result, check=True)

    def test_variable_trip_count_loop_with_exchange_keeps_native_path(self):
        raw = self.assemble(assembly().replace('%value =', 'OpBranch %head\n%head = OpLabel\n%more = OpIEqual %bool %id %one\n'
            'OpLoopMerge %end %cont None\nOpBranchConditional %more %body %end\n%body = OpLabel\n%value =')
            .replace('OpReturn', 'OpBranch %cont\n%cont = OpLabel\nOpBranch %head\n%end = OpLabel\nOpReturn'))
        self.assertEqual(spv_warp.lower(raw), raw)

    def test_shared_words_fit_within_the_vulkan_minimum(self):
        template = assembly(shared=True).replace('%wg = OpTypePointer Workgroup %uint',
            '%count = OpConstant %uint COUNT\n%words = OpTypeArray %uint %count\n%wg = OpTypePointer Workgroup %words')
        # The local size is 64. Its extra 64 scratch words must fit beside the guest arena.
        for count, lowered in ((4032, True), (4033, False)):
            raw = self.assemble(template.replace('COUNT', str(count)))
            portable = spv_warp.lower(raw)
            self.assertEqual(portable != raw, lowered)
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=portable, check=True)

    def test_shared_scratch_uses_its_own_array_layout(self):
        source = assembly().replace('%input =', '%n = OpConstant %uint 64\n%wide = OpTypeArray %uint %n\n%input =')
        source = source.replace('%void =', 'OpDecorate %wide ArrayStride 16\n%void =')
        result = spv_warp.lower(self.assemble(source))
        _, code = decode(result)
        decorated = {i[1] for i in code if opcode(i) == 71 and i[2] == 6}
        self.assertTrue(decorated)
        self.assertFalse(any(i[3] in decorated for i in code if opcode(i) == 32 and i[2] == 4))
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=result, check=True)

    def test_subgroup_mask_and_shuffle_keep_a_consistent_width(self):
        for mask in ('SubgroupEqMask', 'SubgroupGeMask', 'SubgroupGtMask', 'SubgroupLeMask', 'SubgroupLtMask'):
            with self.subTest(mask=mask):
                source = assembly(mask=mask).replace('%value =', '%whole = OpLoad %vec %mask\n%value =')
                portable = spv_warp.lower(self.assemble(source))
                subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=portable, check=True)
                _, code = decode(portable)
                self.assertIn(80, [opcode(i) for i in code])  # vec4(low mask,0,0,0)
                self.assertNotIn(345, [opcode(i) for i in code])
                self.assertFalse(any(opcode(i) == 71 and i[2] == 11 and i[3] in range(4416,4421) for i in code))

    def test_dynamic_component_or_escaped_mask_preserves_the_native_shader(self):
        base = assembly(mask='SubgroupLtMask')
        for address in ('%addr = OpAccessChain %input %mask %id', '%copy = OpCopyObject %mptr %mask'):
            with self.subTest(address=address):
                raw = self.assemble(base.replace('%value =', address + '\n%value ='))
                self.assertEqual(spv_warp.lower(raw), raw)

    def test_signed_whole_mask_and_component_loads_validate(self):
        source = assembly(mask='SubgroupLtMask').replace('%vec = OpTypeVector %uint 4', '%s = OpTypeInt 32 1\n%vec = OpTypeVector %s 4\n%sp = OpTypePointer Input %s')
        source = source.replace('%value =', '%v = OpLoad %vec %mask\n%a = OpAccessChain %sp %mask %one\n%h = OpLoad %s %a\n%value =')
        result = spv_warp.lower(self.assemble(source))
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=result, check=True)
        self.assertNotIn(345, [opcode(i) for i in decode(result)[1]])

    def test_group_decorations_and_duplicate_builtin_variables(self):
        source = assembly(mask='SubgroupLtMask')
        additions = ('%g = OpDecorationGroup\nOpDecorate %g RelaxedPrecision\nOpGroupDecorate %g %mask\n',
                     'OpDecorate %other BuiltIn SubgroupLtMask\n')
        for addition in additions:
            text = source.replace('%void = OpTypeVoid', addition + '%void = OpTypeVoid')
            if '%other' in addition:
                text = text.replace('%main = OpFunction', '%other = OpVariable %mptr Input\n%main = OpFunction')
            raw = self.assemble(text)
            self.assertEqual(spv_warp.lower(raw), raw)

        with_semantic = source.replace('OpMemoryModel', 'OpExtension "SPV_GOOGLE_decorate_string"\nOpExtension "SPV_GOOGLE_hlsl_functionality1"\nOpMemoryModel')
        raw = self.assemble(with_semantic.replace('%void =', 'OpDecorateString %mask UserSemantic "SV_Custom"\n%void ='))
        self.assertEqual(spv_warp.lower(raw), raw)

    def test_mask_read_alone_needs_no_scratch_or_barrier(self):
        source = assembly(shared=True, branch=True, mask='SubgroupEqMask')
        source = source.replace('%value = OpGroupNonUniformShuffle %uint %scope %id %one', '%value = OpLoad %vec %mask')
        raw = self.assemble(source)
        result = spv_warp.lower(raw)
        self.assertNotEqual(result, raw)
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=result, check=True)
        self.assertNotIn(224, [opcode(i) for i in decode(result)[1]])

    def test_converged_shared_warp_barriers_widen_both_scopes(self):
        exchange = '%value = OpGroupNonUniformShuffle %uint %scope %id %one'
        barrier = 'OpControlBarrier %scope %scope %sem'
        for with_exchange in (False, True):
            source = assembly().replace('%input =', '%sem = OpConstant %uint 264\n%input =')
            source = source.replace(exchange, barrier + ('\n' + exchange if with_exchange else ''))
            raw = self.assemble(source)
            portable = spv_warp.lower(raw)
            _, code = decode(portable)
            constants = {i[2]: i[3] for i in code if opcode(i) == 43}
            barriers = [(constants[i[1]], constants[i[2]], constants[i[3]]) for i in code if opcode(i) == 224]
            self.assertEqual(barriers, [(2, 2, 264)] * (3 if with_exchange else 1))
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=portable, check=True)
        source = assembly(branch=True).replace('%input =', '%sem = OpConstant %uint 264\n%input =').replace(exchange, barrier)
        raw = self.assemble(source)
        self.assertEqual(spv_warp.lower(raw), raw)

    def test_physical_subgroup_barrier_in_either_scope_keeps_native_path(self):
        source = assembly().replace('%input =', '%wg = OpConstant %uint 2\n%sem = OpConstant %uint 72\n%input =')
        for barrier in ('OpControlBarrier %wg %scope %sem', 'OpControlBarrier %scope %scope %sem', 'OpMemoryBarrier %scope %sem'):
            raw = self.assemble(source.replace('%value =', barrier + '\n%value ='))
            self.assertEqual(spv_warp.lower(raw), raw)


if __name__ == '__main__':
    unittest.main()
