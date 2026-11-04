using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using System;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Instructions
{
    // Ampere warp matrix instructions (LDSM, MOVM, HMMA), done with exchanges across the warp's 32 lanes (AWarp) and scalar FMAs. Field positions: Mesa NAK
    // sm70_encode.rs.
    // An 8x8 matrix of halves is held by the warp as one register a lane: lane l has row l / 4, columns 2 (l % 4) and 2 (l % 4) + 1
    static partial class InstEmit
    {
        public static void RegisterAmpereMma(AmpereExact x)
        {
            const AmpereProps None = AmpereProps.None;

            x(0x83b, InstName.Lds, ALdsm, None, FeatureFlags.SharedMemory);
            x(0x23a, InstName.Mov, AMovm, None);
            x(0x23c, InstName.Hfma2, AHmma, None);
        }

        private static Operand ALane(EmitterContext context) => EmitLoadSubgroupLaneId(context);

        // The words of the warp's lanes: read(lane 0..31, k) is words[k] of that lane. Through workgroup memory in compute shaders (WarpScratch), else by plain
        // shuffles (the lane is valid by construction: no SHFL helper), in a host subgroup of several warps among the lane's own
        private static Func<Operand, int, Operand> AWarp(EmitterContext context, params Operand[] words)
        {
            if (WarpScratch.Applies(context.TranslatorContext.Definitions.Stage))
            {
                return new WarpScratch(context, context.ResourceManager, context.TranslatorContext.Definitions, words).Read;
            }

            Operand warp = context.TranslatorContext.GpuAccessor.QueryHostSubgroupSize() > 32
                ? context.BitwiseAnd(context.Load(StorageKind.Input, IoVariable.SubgroupLaneId), Const(~31))
                : null;

            return (lane, k) => context.Shuffle(words[k], warp != null ? context.BitwiseOr(lane, warp) : lane);
        }

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
            Func<Operand, int, Operand> rowAddr = AWarp(context, addr);
            Operand[] res = new Operand[count];

            Operand Word(Operand byteAddr) => context.Load(StorageKind.SharedMemory, id, context.ShiftRightU32(byteAddr, Const(2)));

            for (int i = 0; i < count; i++)
            {
                if (!trans)
                {
                    res[i] = Word(context.IAdd(rowAddr(context.IAdd(row, Const(8 * i)), 0), context.ShiftLeft(quad, Const(2))));
                    continue;
                }

                // transposed: rows 2 (l % 4) and 2 (l % 4) + 1 of column l / 4
                Operand r0 = context.IAdd(context.ShiftLeft(quad, Const(1)), Const(8 * i));
                Operand col = context.ShiftLeft(row, Const(1));
                res[i] = APackHalves(context, Word(context.IAdd(rowAddr(r0, 0), col)), Word(context.IAdd(rowAddr(context.IAdd(r0, Const(1)), 0), col)), row);
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
            Func<Operand, int, Operand> src = AWarp(context, AGpr(x.Ra));

            // element (2 (l % 4) + j, l / 4) is in lane 4 (2 (l % 4) + j) + (l / 4) / 2, half (l / 4) % 2
            Operand from = context.IAdd(context.ShiftLeft(quad, Const(3)), context.ShiftRightU32(row, Const(1)));
            ASet(context, x, APackHalves(context, src(from, 0), src(context.IAdd(from, Const(4)), 0), row));
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

            // A's rows r and r + 8 (r = l / 4) come from lanes 4r + q; B's columns 2 (l % 4) and 2 (l % 4) + 1 from lanes 4 (2 (l % 4) + j) + q (q = 0..3). The
            // exchange holds Ra.. (A's k / 4 registers), then Rb.. (B's k / 8)
            Operand[] words = new Operand[k / 4 + k / 8];

            for (int i = 0; i < words.Length; i++)
            {
                words[i] = i < k / 4 ? AGpr(x.Ra, i) : AGpr(x.Rb, i - k / 4);
            }

            Func<Operand, int, Operand> frag = AWarp(context, words);
            Operand[,,] aw = new Operand[2, k / 8, 4], bw = new Operand[2, k / 8, 4];   // the words of A's rows and B's columns with halves 8 h + 2 q and 8 h + 2 q + 1

            for (int q = 0; q < 4; q++)
            {
                Operand aLane = context.IAdd(context.ShiftLeft(row, Const(2)), Const(q));

                for (int h = 0; h < k / 8; h++)
                {
                    for (int i = 0; i < 2; i++)
                    {
                        aw[i, h, q] = frag(aLane, i + 2 * h);
                        bw[i, h, q] = frag(context.IAdd(context.ShiftLeft(quad, Const(3)), Const(4 * i + q)), k / 4 + h);
                    }
                }
            }

            for (int m = 0; m < 2; m++)
            {
                Operand[] d = new Operand[2];

                for (int j = 0; j < 2; j++)
                {
                    // an F16 accumulator sums the pairs' products as halves, C in the low lane, then adds the two lanes (HalfPair); an F32 one in floats
                    Operand c = AGpr(x.Rc, f32 ? 2 * m + j : m);
                    Operand acc = f32 ? c : j == 0 ? context.BitwiseAnd(c, Const(0xffff)) : context.ShiftRightU32(c, Const(16));

                    for (int h = 0; h < k / 8; h++)
                    {
                        for (int q = 0; q < 4; q++)
                        {
                            Operand ab = aw[m, h, q], bb = bw[j, h, q];
                            acc = f32
                                ? context.FPFusedMultiplyAdd(context.UnpackHalf2x16High(ab), context.UnpackHalf2x16High(bb),
                                    context.FPFusedMultiplyAdd(context.UnpackHalf2x16Low(ab), context.UnpackHalf2x16Low(bb), acc))
                                : context.HalfPair(HalfPairOp.FusedMultiplyAdd, ab, bb, acc);
                        }
                    }

                    d[j] = f32 ? acc : context.HalfPair(HalfPairOp.Add, acc, context.BitwiseOr(context.ShiftRightU32(acc, Const(16)), context.ShiftLeft(acc, Const(16))));
                }

                if (f32)
                {
                    ASet(context, x, d[0], 2 * m);
                    ASet(context, x, d[1], 2 * m + 1);
                }
                else
                {
                    ASet(context, x, context.BitwiseOr(context.BitwiseAnd(d[0], Const(0xffff)), context.ShiftLeft(d[1], Const(16))), m);
                }
            }
        }
    }
}
