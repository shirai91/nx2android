using Ryujinx.Common;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation
{
    // nxnative: words passed between a warp's lanes through workgroup memory, in compute shaders, instead of subgroup shuffles (and the bits of ballots,
    // InstEmitWarp EmitBallot). The Adreno 750 stages OpGroupNonUniformShuffle through local memory over the shader's own shared memory: Donkey Kong Bananza's
    // upscaler read its weights back from shared memory with other lanes' shuffled values in their low halves. A lane stores its words at its linear invocation
    // index (word k a workgroup's length further on) and reads lane l of its warp at the warp's first index + l (NVIDIA's lanes are linear index % 32). A
    // subgroup barrier goes before the stores, as the last exchange's reads are done, and after them. The memory is 4-word vectors, so that 4 lanes' words come in
    // one load (Read4)
    readonly struct WarpScratch
    {
        private readonly EmitterContext _context;
        private readonly int _id, _stride;
        private readonly Operand _warp;

        public static bool Applies(ShaderStage stage) => stage == ShaderStage.Compute;

        public WarpScratch(EmitterContext context, ResourceManager resources, ShaderDefinitions definitions, params Operand[] words)
        {
            int sx = definitions.ComputeLocalSizeX, sy = definitions.ComputeLocalSizeY;
            Operand Id(int c) => context.Load(StorageKind.Input, IoVariable.ThreadId, null, Const(c));

            int threads = sx * sy * definitions.ComputeLocalSizeZ;

            _context = context;
            _stride = BitUtils.AlignUp(threads, 4);
            _id = resources.WarpScratchId(_stride * words.Length);

            Operand linear = threads == sx ? Id(0) : context.IAdd(Id(0), context.IMultiply(context.IAdd(Id(1), context.IMultiply(Id(2), Const(sy))), Const(sx)));
            _warp = context.BitwiseAnd(linear, Const(~31));

            context.SubgroupBarrier();

            for (int k = 0; k < words.Length; k++)
            {
                Operand at = context.IAdd(linear, Const(k * _stride));
                context.Store(StorageKind.SharedMemory, _id, context.ShiftRightU32(at, Const(2)), context.BitwiseAnd(at, Const(3)), words[k]);
            }

            context.SubgroupBarrier();
        }

        // Word k of the warp's lane (0..31)
        public Operand Read(Operand lane, int k = 0)
        {
            Operand at = _context.IAdd(_context.IAdd(_warp, lane), Const(k * _stride));

            return _context.Load(StorageKind.SharedMemory, _id, _context.ShiftRightU32(at, Const(2)), _context.BitwiseAnd(at, Const(3)));
        }

        // Word k of the warp's lanes lane4 .. lane4 + 3 (lane4 a multiple of 4)
        public Operand[] Read4(Operand lane4, int k = 0)
        {
            return _context.LoadVector4(StorageKind.SharedMemory, _id, _context.ShiftRightU32(_context.IAdd(_context.IAdd(_warp, lane4), Const(k * _stride)), Const(2)));
        }
    }
}
