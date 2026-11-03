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
        Ampere = 1 << 3,   // NVN2 (Switch 2) code: a 0x80-byte shader header and 128-bit sm_86 instructions
    }
}
