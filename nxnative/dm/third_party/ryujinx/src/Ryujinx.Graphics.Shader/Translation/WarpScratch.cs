using Ryujinx.Common;
using System;
using System.Linq;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation
{
    // nxnative: words passed between a warp's lanes through workgroup memory, in compute shaders, instead of subgroup shuffles (and the bits of ballots,
    // InstEmitWarp EmitBallot). The Adreno 750 stages OpGroupNonUniformShuffle through local memory over the shader's own shared memory: Donkey Kong Bananza's
    // upscaler read its weights back from shared memory with other lanes' shuffled values in their low halves. A lane stores its words at its linear invocation
    // index (word k a workgroup's length further on) and reads lane l of its warp at the warp's first index + l (NVIDIA's lanes are linear index % 32). A
    // subgroup barrier goes after the stores. The memory is 4-word vectors, so that 4 lanes' words come in one load (Read4).
    // There are two buffers: where the emitter tracks which ones the last exchanges' reads may still use (EmitterContext.ScratchBusy, Ampere blocks), an exchange
    // takes a free one, after the barrier of the one before, with no barrier before its stores (half the barriers: they were 3-5 ms of the upscaler's 22);
    // elsewhere it takes buffer 0, after a barrier.
    // The mirror (Mirror) is memory of its own, of a slot a register: words stored there once stay for reads in any later block while the register is unchanged
    // (Donkey Kong Bananza's upscaler: the weights LDSM loads at each of its six stages, that the HMMAs of the five taps after read)
    readonly struct WarpScratch
    {
        private const int MirrorBusy = 4;

        private readonly EmitterContext _context;
        private readonly int _id, _stride, _first, _step;
        private readonly Operand _warp;

        public static bool Applies(ShaderStage stage) => stage == ShaderStage.Compute;

        public WarpScratch(EmitterContext context, ResourceManager resources, ShaderDefinitions definitions, params Operand[] words)
            : this(context, definitions, words, null, Buffer(context), 2, stride => resources.WarpScratchId(stride * 2 * words.Length))
        {
        }

        private WarpScratch(EmitterContext context, ShaderDefinitions definitions, Operand[] words, int[] slots, int first, int step, Func<int, int> memory)
        {
            int sx = definitions.ComputeLocalSizeX, sy = definitions.ComputeLocalSizeY;
            Operand Id(int c) => context.Load(StorageKind.Input, IoVariable.ThreadId, null, Const(c));

            int threads = sx * sy * definitions.ComputeLocalSizeZ;

            _context = context;
            _stride = BitUtils.AlignUp(threads, 4);
            _id = memory(_stride);
            _first = first;
            _step = step;

            Operand linear = threads == sx ? Id(0) : context.IAdd(Id(0), context.IMultiply(context.IAdd(Id(1), context.IMultiply(Id(2), Const(sy))), Const(sx)));
            _warp = context.BitwiseAnd(linear, Const(~31));

            if (words.Length == 0)
            {
                return;
            }

            int busy = slots != null ? MirrorBusy : 1 << first;

            if (context.ScratchBusy < 0 || (context.ScratchBusy & busy) != 0)
            {
                Barrier(context);
            }

            for (int k = 0; k < words.Length; k++)
            {
                Operand at = context.IAdd(linear, Const(Slot(slots?[k] ?? k) * _stride));
                context.Store(StorageKind.SharedMemory, _id, context.ShiftRightU32(at, Const(2)), context.BitwiseAnd(at, Const(3)), words[k]);
            }

            Barrier(context);

            if (context.ScratchBusy >= 0)
            {
                context.ScratchBusy |= slots != null ? 0 : busy;   // (the mirror's reads mark it)
            }
        }

        // The mirror, to read the registers' slots
        public static WarpScratch Mirror(EmitterContext context)
        {
            if (context.ScratchBusy >= 0)
            {
                context.ScratchBusy |= MirrorBusy;
            }

            return new(context, context.TranslatorContext.Definitions, [], null, 0, 1, stride => context.ResourceManager.WarpMirrorId(stride * context.ResourceManager.MirrorSlots));
        }

        // The words of registers regs stored at their slots of the mirror; null where the workgroup memory has no room for them
        public static int[] Mirror(EmitterContext context, int[] regs, Operand[] words)
        {
            ShaderDefinitions definitions = context.TranslatorContext.Definitions;
            ResourceManager resources = context.ResourceManager;
            int slotBytes = BitUtils.AlignUp(definitions.ComputeLocalSizeX * definitions.ComputeLocalSizeY * definitions.ComputeLocalSizeZ, 4) * 4;
            int[] slots = regs.Select(reg => resources.MirrorSlot(reg, slotBytes)).ToArray();

            if (slots.Contains(-1))
            {
                return null;
            }

            _ = new WarpScratch(context, definitions, words, slots, 0, 1, stride => resources.WarpMirrorId(stride * resources.MirrorSlots));

            return slots;
        }

        // The tracked emitter's free buffer (1 when both are, so that exchanges alternate from the first)
        private static int Buffer(EmitterContext context) => context.ScratchBusy < 0 ? 0 : (context.ScratchBusy & 2) == 0 ? 1 : 0;

        private static void Barrier(EmitterContext context)
        {
            context.SubgroupBarrier();

            if (context.ScratchBusy >= 0)
            {
                context.ScratchBusy = 0;
            }
        }

        private int Slot(int k) => k * _step + _first;

        // Word k of the warp's lane (0..31)
        public Operand Read(Operand lane, int k = 0)
        {
            Operand at = _context.IAdd(_context.IAdd(_warp, lane), Const(Slot(k) * _stride));

            return _context.Load(StorageKind.SharedMemory, _id, _context.ShiftRightU32(at, Const(2)), _context.BitwiseAnd(at, Const(3)));
        }

        // Word k of the warp's lanes lane4 .. lane4 + 3 (lane4 a multiple of 4)
        public Operand[] Read4(Operand lane4, int k = 0)
        {
            return _context.LoadVector4(StorageKind.SharedMemory, _id, _context.ShiftRightU32(_context.IAdd(_context.IAdd(_warp, lane4), Const(Slot(k) * _stride)), Const(2)));
        }
    }
}
