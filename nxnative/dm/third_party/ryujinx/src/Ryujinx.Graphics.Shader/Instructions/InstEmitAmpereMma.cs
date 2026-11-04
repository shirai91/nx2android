using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Instructions
{
    // Ampere warp matrix instructions (LDSM, MOVM, HMMA), done with shuffles across the warp's 32 lanes and scalar FMAs. Field positions: Mesa NAK sm70_encode.rs.
    // An 8x8 matrix of halves is held by the warp as one register a lane: lane l has row l / 4, columns 2 (l % 4) and 2 (l % 4) + 1
    static partial class InstEmit
    {
        public static void RegisterAmpereMma(AmpereExact x)
        {
            const AmpereProps None = AmpereProps.None;

            x(0x83b, InstName.Lds, ALdsm, None, FeatureFlags.Shuffle | FeatureFlags.SharedMemory);
            x(0x23a, InstName.Mov, AMovm, None, FeatureFlags.Shuffle);
            x(0x23c, InstName.Hfma2, AHmma, None, FeatureFlags.Shuffle);
        }

        private static Operand ALane(EmitterContext context) => EmitLoadSubgroupLaneId(context);

        private static Operand AShuffle(EmitterContext context, Operand value, Operand lane) => context.Shuffle(value, lane, Const(0x1f)).Item1;

        // The halves of 32-bit words selected by bit 0 of sel (0 low, 1 high), packed as a pair: moved as bits
        private static Operand APackHalves(EmitterContext context, Operand lo, Operand hi, Operand sel)
        {
            Operand shift = context.ShiftLeft(context.BitwiseAnd(sel, Const(1)), Const(4));
            Operand Half(Operand word) => context.BitwiseAnd(context.ShiftRightU32(word, shift), Const(0xffff));

            return context.BitwiseOr(Half(lo), context.ShiftLeft(Half(hi), Const(16)));
        }

        // LDSM Rd, [Ra + UR + offset]: matrices 72..74 (1, 2, 4), .MT88 (78..80 = 1) transposed. Lanes 8i..8i+7 give the row addresses of matrix i; Rd + i gets it
        public static void ALdsm(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x[78..80] > 1 || x[72..74] > 2)
            {
                throw AFail(context, "LDSM with expansion");
            }

            int count = 1 << x[72..74];
            bool trans = x[78..80] == 1;
            Operand addr = context.IAdd(ALocalAddress(context, x, 32, false), Const((int)x.Signed(40..64)));
            Operand lane = ALane(context);
            Operand row = context.ShiftRightU32(lane, Const(2)), quad = context.BitwiseAnd(lane, Const(3));
            int id = context.ResourceManager.SharedMemoryId;
            Operand[] res = new Operand[count];

            Operand Word(Operand byteAddr) => context.Load(StorageKind.SharedMemory, id, context.ShiftRightU32(byteAddr, Const(2)));

            for (int i = 0; i < count; i++)
            {
                if (!trans)
                {
                    res[i] = Word(context.IAdd(AShuffle(context, addr, context.IAdd(row, Const(8 * i))), context.ShiftLeft(quad, Const(2))));
                    continue;
                }

                // transposed: rows 2 (l % 4) and 2 (l % 4) + 1 of column l / 4
                Operand r0 = context.IAdd(context.ShiftLeft(quad, Const(1)), Const(8 * i));
                Operand col = context.ShiftLeft(row, Const(1));
                Operand a0 = context.IAdd(AShuffle(context, addr, r0), col);
                Operand a1 = context.IAdd(AShuffle(context, addr, context.IAdd(r0, Const(1))), col);
                res[i] = APackHalves(context, Word(a0), Word(a1), row);
            }

            for (int i = 0; i < count; i++)
            {
                ASet(context, x, res[i], i);
            }
        }

        // MOVM.MT88 Rd, Ra: the 8x8 matrix transposed
        public static void AMovm(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x[78..80] != 0)
            {
                throw AFail(context, "MOVM other than MT88");
            }

            Operand lane = ALane(context);
            Operand row = context.ShiftRightU32(lane, Const(2)), quad = context.BitwiseAnd(lane, Const(3));
            Operand src = AGpr(x.Ra);

            // element (2 (l % 4) + j, l / 4) is in lane 4 (2 (l % 4) + j) + (l / 4) / 2, half (l / 4) % 2
            Operand from = context.IAdd(context.ShiftLeft(quad, Const(3)), context.ShiftRightU32(row, Const(1)));
            ASet(context, x, APackHalves(context, AShuffle(context, src, from), AShuffle(context, src, context.IAdd(from, Const(4))), row));
        }

        // HMMA.16816 / .1688 Rd, Ra, Rb, Rc: D = A B + C. A (16 x K, row-major) in Ra.. (lane l: rows l / 4 and l / 4 + 8, columns 2 (l % 4).. and 8 more), B (K x 8,
        // column-major) in Rb.. (column l / 4, rows 2 (l % 4).. and 8 more), C and D (16 x 8) in Rc.. and Rd.. (rows l / 4 and l / 4 + 8, columns 2 (l % 4)..): as halves,
        // or as floats (.F32, 76) one register an element. Size 75..76 and 78..79: 0 16x8x8, 1 16x8x16
        public static void AHmma(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int size = x[75..76] | x[78..79] << 1;

            if (size > 1 || x[82..84] != 0)
            {
                throw AFail(context, "HMMA other than F16 16x8x8 / 16x8x16");
            }

            int k = size == 1 ? 16 : 8;
            bool f32 = x[76];
            Operand lane = ALane(context);
            Operand row = context.ShiftRightU32(lane, Const(2)), quad = context.BitwiseAnd(lane, Const(3));

            // A's rows r and r + 8 (r = l / 4) come from lanes 4r + q; B's columns 2 (l % 4) and 2 (l % 4) + 1 from lanes 4 (2 (l % 4) + j) + q (q = 0..3)
            Operand[,] a = new Operand[2, k], b = new Operand[2, k];

            for (int q = 0; q < 4; q++)
            {
                Operand aLane = context.IAdd(context.ShiftLeft(row, Const(2)), Const(q));

                for (int h = 0; h < k / 8; h++)
                {
                    for (int m = 0; m < 2; m++)
                    {
                        Operand w = AShuffle(context, AGpr(x.Ra, m + 2 * h), aLane);
                        a[m, 8 * h + 2 * q] = context.UnpackHalf2x16Low(w);
                        a[m, 8 * h + 2 * q + 1] = context.UnpackHalf2x16High(w);
                    }

                    for (int j = 0; j < 2; j++)
                    {
                        Operand bLane = context.IAdd(context.ShiftLeft(quad, Const(3)), Const(4 * j + q));
                        Operand w = AShuffle(context, AGpr(x.Rb, h), bLane);
                        b[j, 8 * h + 2 * q] = context.UnpackHalf2x16Low(w);
                        b[j, 8 * h + 2 * q + 1] = context.UnpackHalf2x16High(w);
                    }
                }
            }

            Operand[] d = new Operand[4];

            for (int m = 0; m < 2; m++)
            {
                for (int j = 0; j < 2; j++)
                {
                    Operand acc = f32
                        ? AGpr(x.Rc, 2 * m + j)
                        : (j == 0 ? context.UnpackHalf2x16Low(AGpr(x.Rc, m)) : context.UnpackHalf2x16High(AGpr(x.Rc, m)));

                    for (int i = 0; i < k; i++)
                    {
                        acc = context.FPFusedMultiplyAdd(a[m, i], b[j, i], acc);
                    }

                    d[2 * m + j] = acc;
                }
            }

            for (int m = 0; m < 2; m++)
            {
                if (f32)
                {
                    ASet(context, x, d[2 * m], 2 * m);
                    ASet(context, x, d[2 * m + 1], 2 * m + 1);
                }
                else
                {
                    ASet(context, x, context.PackHalf2x16(d[2 * m], d[2 * m + 1]), m);
                }
            }
        }
    }
}
