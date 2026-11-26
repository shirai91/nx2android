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

        // nxnative: callers must establish the original vertex interface first.
        OptimizeConstantBufferAddress = 1 << 3,
    }
}
