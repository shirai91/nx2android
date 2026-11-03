namespace Ryujinx.Graphics.Shader.Decoders
{
    enum RegisterType
    {
        Flag,
        Gpr,
        Predicate,
        Uniform,            // Ampere UR0-UR62; UR63 (URZ) reads as zero
        UniformPredicate,   // Ampere UP0-UP6; UP7 (UPT) reads as true
        Shadow,             // Ampere results held back until their scoreboard is waited on (InstEmitAmpere.cs)
    }
}
