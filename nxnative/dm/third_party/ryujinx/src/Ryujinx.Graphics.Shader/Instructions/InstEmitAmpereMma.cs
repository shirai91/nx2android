using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.Linq;
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

        // The words of the warp's lanes: Read(lane 0..31, k) is words[k] of that lane, Read4(lane4, k) of lanes lane4 .. lane4 + 3. Through workgroup memory in
        // compute shaders (WarpScratch), else by plain shuffles (the lane is valid by construction: no SHFL helper), in a host subgroup of several warps among the
        // lane's own
        private readonly record struct AWarpWords(Func<Operand, int, Operand> Read, Func<Operand, int, Operand[]> Read4);

        private static AWarpWords AWarp(EmitterContext context, params Operand[] words)
        {
            if (WarpScratch.Applies(context.TranslatorContext.Definitions.Stage))
            {
                WarpScratch scratch = new(context, context.ResourceManager, context.TranslatorContext.Definitions, words);

                return new(scratch.Read, scratch.Read4);
            }

            Operand warp = context.TranslatorContext.GpuAccessor.QueryHostSubgroupSize() > 32
                ? context.BitwiseAnd(context.Load(StorageKind.Input, IoVariable.SubgroupLaneId), Const(~31))
                : null;
            Operand Read(Operand lane, int k) => context.Shuffle(words[k], warp != null ? context.BitwiseOr(lane, warp) : lane);

            return new(Read, (lane4, k) => [Read(lane4, k), Read(context.IAdd(lane4, Const(1)), k), Read(context.IAdd(lane4, Const(2)), k), Read(context.IAdd(lane4, Const(3)), k)]);
        }

        // The kinds of lane sets cross-lane reads take a register's words from (AFragments): HMMA's A (lanes 4 (l / 4) .. + 3), B (4 (2 (l % 4) + i) .. + 3, i = 0, 1),
        // MOVM's (8 (l % 4) + (l / 4) / 2 and + 4), LDSM's row addresses (matrix i: l / 4 + 8 i; transposed rows 2 (l % 4) + 8 i and + 1)
        private const int FragA = 0, FragB = 1, FragMovm = 3, FragLdsm = 4, FragLdsmT = 8;

        // Registers' words of other lanes, as (register, kind) keys want. Words an earlier instruction of the block read come from its cache (EmitterContext.Fragments,
        // emptied where a register is written); the others in one exchange, with those the run of like instructions after this one wants (MOVMs after a MOVM,
        // LDSMs after an LDSM, while their sources are not written): a tile's HMMAs share their registers, and MOVMs and LDSMs come in fours (Donkey Kong Bananza's
        // upscaler: each exchange costs a store's and a load's latency on the Adreno 750, and its 144 of LDSM and MOVM took 4.5 ms of its 18)
        private static Dictionary<(int, int), Operand[]> AFragments(EmitterContext context, List<(int Reg, int Kind)> keys)
        {
            AmpereOp x = new(context.CurrOp);
            Dictionary<(int, int), Operand[]> cache = context.Fragments, res = new();
            List<(int Reg, int Kind)> missing = keys.Where(key => !cache.ContainsKey(key)).Distinct().ToList();

            if (missing.Count != 0)
            {
                List<InstOp> code = context.CurrBlock.OpCodes;
                HashSet<int> written = [];

                for (int n = code.IndexOf(context.CurrOp); x.Guard == Pt && n >= 0 && ++n < code.Count;)   // (the run's words are cached, so an unguarded instruction's only)
                {
                    AmpereOp y = new(code[n]);

                    if (y.Op != x.Op || x.Op == 0x23c)
                    {
                        break;
                    }

                    AmpereOp prev = new(code[n - 1]);
                    written.UnionWith(Enumerable.Range(prev.Rd, x.Op == 0x83b ? 1 << prev[72..74] : 1));

                    if (!written.Contains(y.Ra))
                    {
                        int count = x.Op == 0x83b ? 1 << y[72..74] : 1;
                        missing.AddRange(Enumerable.Range(0, count).Select(i => (y.Ra, x.Op == 0x23a ? FragMovm : (y[78..80] == 1 ? FragLdsmT : FragLdsm) + i)).Where(key => !cache.ContainsKey(key) && !missing.Contains(key)));
                    }
                }

                int[] regs = missing.Select(m => m.Reg).Distinct().ToArray();
                AWarpWords words = AWarp(context, regs.Select(r => AGpr(r)).ToArray());
                Operand lane = ALane(context);
                Operand row = context.ShiftRightU32(lane, Const(2)), quad = context.BitwiseAnd(lane, Const(3));

                foreach ((int reg, int kind) in missing)
                {
                    int w = Array.IndexOf(regs, reg);
                    Operand at(int l) => context.IAdd(context.ShiftLeft(quad, Const(1)), Const(l));

                    res[(reg, kind)] = kind switch
                    {
                        FragA => words.Read4(context.ShiftLeft(row, Const(2)), w),
                        FragB or FragB + 1 => words.Read4(context.IAdd(context.ShiftLeft(quad, Const(3)), Const(4 * (kind - FragB))), w),
                        FragMovm => [words.Read(context.IAdd(context.ShiftLeft(quad, Const(3)), context.ShiftRightU32(row, Const(1))), w),
                                     words.Read(context.IAdd(context.IAdd(context.ShiftLeft(quad, Const(3)), context.ShiftRightU32(row, Const(1))), Const(4)), w)],
                        < FragLdsmT => [words.Read(context.IAdd(row, Const(8 * (kind - FragLdsm))), w)],
                        _ => [words.Read(at(8 * (kind - FragLdsmT)), w), words.Read(at(8 * (kind - FragLdsmT) + 1), w)],
                    };

                    if (x.Guard == Pt)   // (a guarded instruction's words do not dominate the block's later code)
                    {
                        cache[(reg, kind)] = res[(reg, kind)];
                    }
                }
            }

            foreach ((int reg, int kind) key in keys)
            {
                res.TryAdd(key, cache.GetValueOrDefault(key));
            }

            return res;
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
            Operand uniform = context.ISubtract(context.IAdd(ALocalAddress(context, x, 32, false), Const((int)x.Signed(40..64))), AGpr(x.Ra));   // the address but Ra, alike in every lane
            Operand lane = ALane(context);
            Operand row = context.ShiftRightU32(lane, Const(2)), quad = context.BitwiseAnd(lane, Const(3));
            int id = context.ResourceManager.SharedMemoryId;
            Dictionary<(int, int), Operand[]> ra = AFragments(context, Enumerable.Range(0, count).Select(i => (x.Ra, (trans ? FragLdsmT : FragLdsm) + i)).ToList());
            Operand[] res = new Operand[count];

            Operand Word(Operand rowAddr, Operand at) => context.Load(StorageKind.SharedMemory, id, context.ShiftRightU32(context.IAdd(context.IAdd(rowAddr, uniform), at), Const(2)));

            for (int i = 0; i < count; i++)
            {
                Operand[] w = ra[(x.Ra, (trans ? FragLdsmT : FragLdsm) + i)];

                // transposed: rows 2 (l % 4) and 2 (l % 4) + 1 of column l / 4
                res[i] = trans
                    ? APackHalves(context, Word(w[0], context.ShiftLeft(row, Const(1))), Word(w[1], context.ShiftLeft(row, Const(1))), row)
                    : Word(w[0], context.ShiftLeft(quad, Const(2)));
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

            Operand[] src = AFragments(context, [(x.Ra, FragMovm)])[(x.Ra, FragMovm)];
            ASet(context, x, APackHalves(context, src[0], src[1], context.ShiftRightU32(ALane(context), Const(2))));
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

            Dictionary<(int, int), Operand[]> frag = AFragments(context, Enumerable.Range(0, k / 8).SelectMany(h => new[] { (x.Ra + 2 * h, FragA), (x.Ra + 2 * h + 1, FragA), (x.Rb + h, FragB), (x.Rb + h, FragB + 1) }).ToList());
            Operand[] Words((int, int) key) => frag[key];
            Operand[,][] aw = new Operand[2, k / 8][], bw = new Operand[2, k / 8][];   // the words of A's rows and B's columns, [q] with halves 8 h + 2 q and + 1

            for (int h = 0; h < k / 8; h++)
            {
                for (int i = 0; i < 2; i++)
                {
                    aw[i, h] = Words((x.Ra + i + 2 * h, 0));
                    bw[i, h] = Words((x.Rb + h, 1 + i));
                }
            }

            // fp32 FMAs for an F16 accumulator too, rounded to halves once: on the Adreno 750 fp32 FMAs run at twice the rate of fp16 ones and four times f16vec2's
            for (int m = 0; m < 2; m++)
            {
                Operand[] d = new Operand[2];

                for (int j = 0; j < 2; j++)
                {
                    Operand acc = f32 ? AGpr(x.Rc, 2 * m + j) : j == 0 ? context.UnpackHalf2x16Low(AGpr(x.Rc, m)) : context.UnpackHalf2x16High(AGpr(x.Rc, m));

                    for (int h = 0; h < k / 8; h++)
                    {
                        for (int q = 0; q < 4; q++)
                        {
                            Operand ab = aw[m, h][q], bb = bw[j, h][q];
                            acc = context.FPFusedMultiplyAdd(context.UnpackHalf2x16High(ab), context.UnpackHalf2x16High(bb),
                                context.FPFusedMultiplyAdd(context.UnpackHalf2x16Low(ab), context.UnpackHalf2x16Low(bb), acc));
                        }
                    }

                    d[j] = acc;
                }

                if (f32)
                {
                    ASet(context, x, d[0], 2 * m);
                    ASet(context, x, d[1], 2 * m + 1);
                }
                else
                {
                    ASet(context, x, context.PackHalf2x16Rte(d[0], d[1]), m);
                }
            }
        }
    }
}
