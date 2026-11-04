using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation
{
    // nxnative: words passed between a warp's lanes through workgroup memory, in compute shaders, instead of subgroup shuffles (and the bits of ballots,
    // InstEmitWarp EmitBallot). The Adreno 750 stages OpGroupNonUniformShuffle through local memory over the shader's own shared memory: Donkey Kong Bananza's
    // upscaler read its weights back from shared memory with other lanes' shuffled values in their low halves. A lane stores its words at its linear invocation
    // index (word k a workgroup's length further on) and reads lane l of its warp at the warp's first index + l (NVIDIA's lanes are linear index % 32). A
    // subgroup barrier goes before the stores, as the last exchange's reads are done, and after them
    readonly struct WarpScratch
    {
        private readonly EmitterContext _context;
        private readonly int _id, _threads;
        private readonly Operand _warp;

        public static bool Applies(ShaderStage stage) => stage == ShaderStage.Compute;

        public WarpScratch(EmitterContext context, ResourceManager resources, ShaderDefinitions definitions, params Operand[] words)
        {
            int sx = definitions.ComputeLocalSizeX, sy = definitions.ComputeLocalSizeY;
            Operand Id(int c) => context.Load(StorageKind.Input, IoVariable.ThreadId, null, Const(c));

            _context = context;
            _threads = sx * sy * definitions.ComputeLocalSizeZ;
            _id = resources.WarpScratchId(_threads * words.Length);

            Operand linear = _threads == sx ? Id(0) : context.IAdd(Id(0), context.IMultiply(context.IAdd(Id(1), context.IMultiply(Id(2), Const(sy))), Const(sx)));
            _warp = context.BitwiseAnd(linear, Const(~31));

            context.SubgroupBarrier();

            for (int k = 0; k < words.Length; k++)
            {
                context.Store(StorageKind.SharedMemory, _id, context.IAdd(linear, Const(k * _threads)), words[k]);
            }

            context.SubgroupBarrier();
        }

        // Word k of the warp's lane (0..31)
        public Operand Read(Operand lane, int k = 0)
        {
            return _context.Load(StorageKind.SharedMemory, _id, _context.IAdd(_context.IAdd(_warp, lane), Const(k * _threads)));
        }
    }
}
