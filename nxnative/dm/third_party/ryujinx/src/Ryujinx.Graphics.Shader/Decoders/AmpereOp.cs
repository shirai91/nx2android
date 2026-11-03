using System;

namespace Ryujinx.Graphics.Shader.Decoders
{
    // One 128-bit Ampere (sm_86) instruction. Fields are addressed by bit ranges as Mesa NAK's encoder (sm70_encode.rs) writes them: x[40..54], x[91].
    readonly struct AmpereOp
    {
        public readonly ulong Lo;
        public readonly ulong Hi;

        public AmpereOp(InstOp op)
        {
            Lo = op.RawOpCode;
            Hi = op.RawOpCodeHi;
        }

        public AmpereOp(ulong lo, ulong hi)
        {
            Lo = lo;
            Hi = hi;
        }

        public int this[Range r] => (int)Field(r);

        public bool this[int bit] => ((bit < 64 ? Lo >> bit : Hi >> (bit - 64)) & 1) != 0;

        public ulong Field(Range r)
        {
            int lo = r.Start.Value, n = r.End.Value - lo;
            UInt128 v = (((UInt128)Hi << 64) | Lo) >> lo;
            return (ulong)(v & ((UInt128.One << n) - 1));
        }

        public long Signed(Range r)
        {
            int n = r.End.Value - r.Start.Value;
            return (long)(Field(r) << (64 - n)) >> (64 - n);
        }

        public int Op => (int)(Lo & 0xfff);
        public int Form => this[9..12];
        public int Rd => this[16..24];
        public int Ra => this[24..32];
        public int Rb => this[32..40];
        public int Rc => this[64..72];
        public int Imm32 => (int)(Lo >> 32);

        // The guard: P (or UP on the uniform datapath) at 12..15, inverted by 15
        public int Guard => this[12..15];
        public bool GuardNot => this[15];

        // Scoreboards: the barrier this instruction's results arrive on (7: none), and the barriers it waits for first
        public int WriteBarrier => this[110..113];
        public int WaitMask => this[116..122];

        // BRA / CALL / BRX: a signed word offset relative to the next instruction
        public long BranchOffset => Signed(34..82) * 4;
    }
}
