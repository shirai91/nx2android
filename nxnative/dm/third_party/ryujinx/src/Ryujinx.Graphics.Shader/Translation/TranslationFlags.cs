using System;

namespace Ryujinx.Graphics.Shader.Translation
{
    [Flags]
    public enum TranslationFlags
    {
        None = 0,

        VertexA = 1 << 0,
        Compute = 1 << 1,
        DebugMode = 1 << 2,
        // nxnative: a read of a game uniform slot (c[3] to c[16]) past the size the game bound gives 0, as Maxwell's constant buffers do (seedds/nxnative's
        // MetalUniformBounds). A read at a computed offset is compared with the size (Constants.NxUniformSizesBank); the reads at fixed offsets are reported as an
        // extent of the slot (BufferDescriptor.FixedSize), which the host makes readable
        UniformBounds = 1 << 3,
    }
}
