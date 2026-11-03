using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using System;
using static Ryujinx.Graphics.Shader.Instructions.InstEmitAluHelper;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Instructions
{
    delegate void AmpereAlu(int op, InstName name, InstEmitter emitter, AmpereProps props);
    delegate void AmpereExact(int op, InstName name, InstEmitter emitter, AmpereProps props, FeatureFlags features = FeatureFlags.None);

    // Ampere ALU, conversion, move and predicate instructions. Field positions: Mesa NAK sm70_encode.rs. The uniform-datapath twins (0x080 set in the 9-bit
    // opcode) share the emitters.
    static partial class InstEmit
    {
        public static void RegisterAmpereAlu(AmpereAlu alu, AmpereExact x)
        {
            const AmpereProps N = AmpereProps.NoRd, U = AmpereProps.Uniform, P = AmpereProps.Pure, None = AmpereProps.None;

            alu(0x020, InstName.Fmul, AFmul, None);
            alu(0x021, InstName.Fadd, AFadd, None);
            alu(0x023, InstName.Ffma, AFfma, None);
            alu(0x009, InstName.Fmnmx, AFmnmx, None);
            alu(0x00a, InstName.Fset, AFset, None);
            alu(0x00b, InstName.Fsetp, AFsetp, N);
            alu(0x008, InstName.Fsel, AFsel, None);
            alu(0x108, InstName.Mufu, AMufu, None);
            alu(0x104, InstName.F2f, AF2f, None);
            alu(0x105, InstName.F2i, AF2i, None);
            alu(0x106, InstName.I2f, AI2f, None);
            alu(0x107, InstName.Frnd, AFrnd, None);
            alu(0x045, InstName.I2f, AI2fp, None);
            alu(0x03e, InstName.F2fp, AF2fp, None);
            alu(0x030, InstName.Hadd2, AHalf, None);
            alu(0x031, InstName.Hfma2, AHalf, None);
            alu(0x032, InstName.Hmul2, AHalf, None);
            alu(0x034, InstName.Hsetp2, AHsetp2, N);
            alu(0x040, InstName.Hmul2, AHmnmx2, None);
            alu(0x013, InstName.Iabs, AIabs, None);
            alu(0x024, InstName.Imad, AImad, None);
            alu(0x025, InstName.Imad, AImad, None);
            alu(0x027, InstName.Imad, AImad, None);
            alu(0x109, InstName.Popc, ABits, None);
            alu(0x100, InstName.Flo, ABits, None);
            alu(0x101, InstName.Brev, ABits, None);

            // vector and uniform datapath
            foreach (int u in new[] { 0, 0x80 })
            {
                AmpereProps props = u == 0 ? None : U;

                alu(0x002 + u, InstName.Mov, AMov, props);
                alu(0x007 + u, InstName.Sel, ASel, props);
                alu(0x00c + u, InstName.Isetp, AIsetp, props | N);
                alu(0x010 + u, InstName.Iadd3, AIadd3, props);
                alu(0x011 + u, InstName.Lea, ALea, props);
                alu(0x012 + u, InstName.Lop3, ALop3, props);
                alu(0x016 + u, InstName.Prmt, APrmt, props);
                alu(0x017 + u, InstName.Imnmx, AImnmx, props);
                alu(0x019 + u, InstName.Shf, AShf, props);
                alu(0x01a + u, InstName.Sgxt, ASgxt, props);
            }

            alu(0x0a4, InstName.Imad, AImad, U);
            alu(0x0a5, InstName.Imad, AImad, U);
            alu(0x085, InstName.Imnmx, AImnmx, U);
            alu(0x0bf, InstName.Popc, ABits, U);
            alu(0x0bd, InstName.Flo, ABits, U);
            alu(0x0be, InstName.Brev, ABits, U);

            x(0x882, InstName.Mov, AMov, P | U);
            x(0xc82, InstName.Mov, AMov, P | U);
            x(0x3c2, InstName.Mov, AR2ur, P);
            x(0x805, InstName.Cs2r, ACs2r, P);
            x(0x919, InstName.S2r, AS2r, P);
            x(0x9c3, InstName.S2r, AS2r, P | U);
            x(AmpereTable.Ldc, InstName.Ldc, ALdc, P);
            x(AmpereTable.Uldc, InstName.Ldc, ALdc, P | U);
            x(AmpereTable.UldcIndexed, InstName.Ldc, ALdc, P | U);
            x(0x822, InstName.Fswzadd, AFswzadd, P);
            x(0x804, InstName.R2p, AR2p, P | N);
            x(0x204, InstName.R2p, AR2p, P | N);
            x(0x803, InstName.P2r, AP2r, P);
            x(0x203, InstName.P2r, AP2r, P);
            x(0x81c, InstName.Psetp, APlop3, P | N);
            x(0x89c, InstName.Psetp, APlop3, P | N | U);
            x(0x806, InstName.Vote, AVote, None);
            x(0x886, InstName.Vote, AVote, None);

            foreach (int op in new[] { 0x389, 0x589, 0x989, 0xf89 })
            {
                x(op, InstName.Shfl, AShfl, None, FeatureFlags.Shuffle);
            }
        }

        // ---- float ----

        public static void AFadd(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            ASet(context, x, context.FPSaturate(context.FPAdd(AF(context, x, SA), AF(context, x, S32)), x[77]));
        }

        // FMUL: .FMZ (76), the product scaled by 2^(84..87 - 4)
        public static void AFmul(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            Operand a = AF(context, x, SA), b = AF(context, x, S32);
            Operand res = AFmz(context, x, a, b, context.FPMultiply(a, b), ConstF(0));
            int scale = x[84..87];

            if (scale != 4)
            {
                res = context.FPMultiply(res, ConstF(MathF.ScaleB(1f, scale - 4)));
            }

            ASet(context, x, context.FPSaturate(res, x[77]));
        }

        public static void AFfma(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            Operand a = AF(context, x, SA), b = AF(context, x, SlotB(x)), c = AF(context, x, SlotC(x));
            ASet(context, x, context.FPSaturate(AFmz(context, x, a, b, context.FPFusedMultiplyAdd(a, b, c), c), x[77]));
        }

        // .FMZ (76): a product with a zero factor is zero, whatever the other (Direct3D 9's rule: 0 * Inf = 0)
        private static Operand AFmz(EmitterContext context, AmpereOp x, Operand a, Operand b, Operand res, Operand zeroProduct)
        {
            return x[76] ? context.ConditionalSelect(context.BitwiseOr(context.FPCompareEqual(a, ConstF(0)), context.FPCompareEqual(b, ConstF(0))), zeroProduct, res) : res;
        }

        // FMNMX: the minimum where P (87..90) holds
        public static void AFmnmx(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            Operand a = AF(context, x, SA), b = AF(context, x, S32);
            ASet(context, x, context.ConditionalSelect(APred(context, x, 87, 90), context.FPMinimum(a, b), context.FPMaximum(a, b)));
        }

        // FSET: 1.0 where (a cmp b) bop P holds; FSETP: Pd0 = (a cmp b) bop P, Pd1 = !(a cmp b) bop P. Comparison 76..80, bop 74..76
        public static void AFset(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            ASet(context, x, context.ConditionalSelect(AFCompare(context, x, false), ConstF(1), Const(0)));
        }

        public static void AFsetp(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            ASetPred(context, x[81..84], AFCompare(context, x, false));
            ASetPred(context, x[84..87], AFCompare(context, x, true));
        }

        private static Operand AFCompare(EmitterContext context, AmpereOp x, bool not)
        {
            Operand cmp = GetFPComparison(context, (FComp)x[76..80], AF(context, x, SA), AF(context, x, S32));
            return GetPredLogicalOp(context, (BoolOp)x[74..76], not ? context.BitwiseNot(cmp) : cmp, APred(context, x, 87, 90));
        }

        public static void AFsel(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            ASet(context, x, context.ConditionalSelect(APred(context, x, 87, 90), AF(context, x, SA), AF(context, x, S32)));
        }

        // MUFU: operation 74..80; .F16 (73) reads the half 60..62 selects and writes the low half. The trigonometric argument is in turns (Volta and later)
        public static void AMufu(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool half = x[73];
            Operand src = half ? AHalves(context, x, S32, x[60..62])[0] : AF(context, x, S32);

            Operand res = x[74..80] switch
            {
                0 => context.FPCosine(context.FPMultiply(src, ConstF(2f * MathF.PI))),
                1 => context.FPSine(context.FPMultiply(src, ConstF(2f * MathF.PI))),
                2 => context.FPExponentB2(src),
                3 => context.FPLogarithmB2(src),
                4 => context.FPReciprocal(src),
                5 => context.FPReciprocalSquareRoot(src),
                6 => context.UnpackDouble2x32High(context.FPReciprocal(context.PackDouble2x32(Const(0), src), Instruction.FP64)),
                7 => context.UnpackDouble2x32High(context.FPReciprocalSquareRoot(context.PackDouble2x32(Const(0), src), Instruction.FP64)),
                8 => context.FPSquareRoot(src),
                int op => throw AFail(context, $"MUFU {op}"),
            };

            ASet(context, x, half ? context.PackHalf2x16(res, ConstF(0)) : res);
        }

        // ---- conversions: source size 84..86 and destination size 75..77 (log2 bytes), rounding 78..80, signed 72 (F2I) / 74 (I2F) ----

        public static void AF2i(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int size = x[75..77];

            if (x[84..86] != 2 || size > 2)
            {
                throw AFail(context, "F2I size");
            }

            (bool neg, bool abs) = AMods(x, S32);
            EmitF2I(context, DstFmt.F32, (IDstFmt)((x[72] ? 4 : 0) | Math.Max(size, 1)), (RoundMode2)x[78..80], ASrc(context, x, S32), x.Rd, abs, neg);

            if (size == 0 && x.Rd != Rz)
            {
                Operand r = Register(x.Rd, RegisterType.Gpr);
                context.Copy(r, x[72] ? context.IClampS32(r, Const(-128), Const(127)) : context.IClampU32(r, Const(0), Const(255)));
            }
        }

        public static void AI2f(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int srcSize = x[84..86];

            if (srcSize > 2 || x[75..77] is not (1 or 2))
            {
                throw AFail(context, "I2F size");
            }

            int sel = srcSize == 1 ? x[60..62] * 2 : x[60..62];
            EmitI2F(context, (ISrcFmt)((x[74] ? 4 : 0) | srcSize), (DstFmt)x[75..77], ASrc(context, x, S32), (ByteSel)sel, x.Rd, false, false);
        }

        // I2FP.F32.S32 / .U32 (signed 74)
        public static void AI2fp(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            Operand src = ASrc(context, x, S32);
            ASet(context, x, x[74] ? context.IConvertS32ToFP32(src) : context.IConvertU32ToFP32(src));
        }

        public static void AFrnd(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x[84..86] != 2 || x[75..77] != 2)
            {
                throw AFail(context, "FRND size");
            }

            (bool neg, bool abs) = AMods(x, S32);
            EmitF2F(context, DstFmt.F32, DstFmt.F32, (IntegerRound)(x[78..80] + 4), ASrc(context, x, S32), x.Rd, abs, neg, false);
        }

        // F2F between half, single and double precision; a half source is the half 60..62 selects
        public static void AF2f(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int srcSize = x[84..86], dstSize = x[75..77];

            if (srcSize is < 1 or > 3 || dstSize is < 1 or > 3)
            {
                throw AFail(context, "F2F size");
            }

            Operand src = srcSize switch
            {
                1 => x[60..62] is 1 or 3 ? context.UnpackHalf2x16High(ASrc(context, x, S32)) : context.UnpackHalf2x16Low(ASrc(context, x, S32)),
                3 => context.PackDouble2x32(ASrc(context, x, S32), ASrcHigh(context, x, S32)),
                _ => ASrc(context, x, S32),
            };

            (bool neg, bool abs) = AMods(x, S32);
            EmitF2F(context, (DstFmt)Math.Max(srcSize, 2), (DstFmt)dstSize, IntegerRound.Pass, src, x.Rd, abs, neg, false);
        }

        // F2FP.F16.F32.PACK_AB: the 32-slot operand to the low half, A to the high one
        public static void AF2fp(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x[72] || x[78])
            {
                throw AFail(context, "F2FP.MERGE_C");
            }

            ASet(context, x, context.PackHalf2x16(AF(context, x, S32), AF(context, x, SA)));
        }

        // ---- packed halves ----

        // A half pair's two lanes after its swizzle (0 as is, 2 low twice, 3 high twice, 1 the high half twice); immediates and constants take none
        private static Operand[] AHalves(EmitterContext context, AmpereOp x, int slot, int swizzle)
        {
            Operand v = ASrc(context, x, slot);
            bool plain = slot == S32 && x.Form is not (1 or 6);

            return (plain ? 0 : swizzle) switch
            {
                0 => [context.UnpackHalf2x16Low(v), context.UnpackHalf2x16High(v)],
                2 => [context.UnpackHalf2x16Low(v), context.UnpackHalf2x16Low(v)],
                _ => [context.UnpackHalf2x16High(v), context.UnpackHalf2x16High(v)],
            };
        }

        // HADD2 / HMUL2 / HFMA2: lanes of A (swizzle 74..76, neg 72, abs 73), the 32 slot (60..62, 63, 62) and the 64 slot (81..83, 84, 83). .F32 (78) writes the
        // low lane as one float; .RELU (79, HMUL2 / HFMA2) clamps at zero
        public static void AHalf(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x[85])
            {
                throw AFail(context, "BF16 half pair");
            }

            Operand[] Lanes(int slot, int swizzle, (bool Neg, bool Abs) m) => InstEmitHelper.FPAbsNeg(context, AHalves(context, x, slot, swizzle), m.Abs, m.Neg);

            Operand[] a = Lanes(SA, x[74..76], AMods(x, SA));
            Operand[] s32 = Lanes(S32, x[60..62], AMods(x, S32));
            Operand[] s64 = Lanes(S64, x[81..83], (x[84], x[83]));
            (Operand[] b, Operand[] c) = (x.Op & 0x1ff) != 0x031 || SlotB(x) == S32 ? (s32, s64) : (s64, s32);   // HADD2 / HMUL2 take their second operand from the 32 slot in every form
            Operand[] res = new Operand[2];

            for (int i = 0; i < 2; i++)
            {
                res[i] = (x.Op & 0x1ff) switch
                {
                    0x030 => context.FPAdd(a[i], b[i]),
                    0x032 => context.FPMultiply(a[i], b[i]),
                    _ => context.FPFusedMultiplyAdd(a[i], b[i], c[i]),
                };

                if (x[79] && (x.Op & 0x1ff) != 0x030)
                {
                    res[i] = context.FPMaximum(res[i], ConstF(0));
                }

                res[i] = context.FPSaturate(res[i], x[77]);
            }

            ASet(context, x, x[78] ? res[0] : context.PackHalf2x16(res[0], res[1]));
        }

        // HSETP2: each lane compared (76..80) and combined (69..71) with the predicate 87..90 into 81..84 (low) and 84..87 (high); .H_AND (71) gives the first
        // both lanes' AND and the second its negation
        public static void AHsetp2(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            Operand[] a = InstEmitHelper.FPAbsNeg(context, AHalves(context, x, SA, x[74..76]), AMods(x, SA).Abs, AMods(x, SA).Neg);
            Operand[] b = InstEmitHelper.FPAbsNeg(context, AHalves(context, x, S32, x[60..62]), AMods(x, S32).Abs, AMods(x, S32).Neg);
            Operand lo = GetFPComparison(context, (FComp)x[76..80], a[0], b[0]), hi = GetFPComparison(context, (FComp)x[76..80], a[1], b[1]);
            Operand acc = APred(context, x, 87, 90);
            BoolOp op = (BoolOp)x[69..71];

            if (x[71])
            {
                lo = context.BitwiseAnd(lo, hi);
                hi = context.BitwiseNot(lo);
            }

            ASetPred(context, x[81..84], GetPredLogicalOp(context, op, lo, acc));
            ASetPred(context, x[84..87], GetPredLogicalOp(context, op, hi, acc));
        }

        // HMNMX2: lane by lane the minimum where the predicate 87..90 holds, else the maximum
        public static void AHmnmx2(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x[78] || x[81] || x[82] || x[85])
            {
                throw AFail(context, "HMNMX2 .F32 / .NAN / .XORSIGN / BF16");
            }

            Operand[] a = InstEmitHelper.FPAbsNeg(context, AHalves(context, x, SA, x[74..76]), AMods(x, SA).Abs, AMods(x, SA).Neg);
            Operand[] b = InstEmitHelper.FPAbsNeg(context, AHalves(context, x, S32, x[60..62]), AMods(x, S32).Abs, AMods(x, S32).Neg);
            Operand min = APred(context, x, 87, 90);
            Operand Lane(int i) => context.ConditionalSelect(min, context.FPMinimum(a[i], b[i]), context.FPMaximum(a[i], b[i]));
            ASet(context, x, context.PackHalf2x16(Lane(0), Lane(1)));
        }

        // ---- moves ----

        public static void AMov(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            ASet(context, x, ASrc(context, x, S32));
        }

        public static void AR2ur(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            ASet(context, Register(x[16..22], RegisterType.Uniform), AGpr(x.Ra));
        }

        public static void ASel(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            ASet(context, x, context.ConditionalSelect(APred(context, x, 87, 90), ASrc(context, x, SA), ASrc(context, x, S32)));
        }

        // CS2R Rd, SR (72..80; 255 is zero), .64 (80) two registers
        public static void ACs2r(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int sr = x[72..80];

            for (int k = 0; k < (x[80] ? 2 : 1) && x.Rd + k < Rz; k++)
            {
                ASet(context, x, sr == 255 ? Const(0) : EmitSReg(context, (SReg)(sr + k)), k);
            }
        }

        // S2R Rd / S2UR URd, SR (72..80)
        public static void AS2r(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            ASet(context, x, EmitSReg(context, (SReg)x[72..80]));
        }

        // LDC Rd, c[bank][Ra + offset] / ULDC URd, c[bank][offset] / ULDC URd, c[bank][URa + offset] (0xabb, its 91 set; bank 54..59, offset 38..54, size 73..76:
        // a byte, short, word or two words). 78..80 address modes and a bank in a uniform register (91 of the others) are not met
        public static void ALdc(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int size = x[73..76];

            if ((x[91] && x.Op != AmpereTable.UldcIndexed) || x[78..80] != 0 || size > 5)
            {
                throw AFail(context, "LDC form");
            }

            Operand index = x.Op switch
            {
                AmpereTable.Ldc => AGpr(x.Ra),
                AmpereTable.UldcIndexed => Register(x[24..30], RegisterType.Uniform),
                _ => Const(0),
            };

            (int bank, int offset) = index.Type == OperandType.Constant ? AMapC0(context, x[54..59], (int)x.Signed(38..54)) : (x[54..59], (int)x.Signed(38..54));

            // A word at a known offset is a constant operand, as an ALU instruction's: the passes that find storage buffer addresses look for those
            if (index.Type == OperandType.Constant && size >= 4 && (offset & 3) == 0)
            {
                for (int k = 0; k < (size == 5 ? 2 : 1); k++)
                {
                    ASet(context, x, Cbuf(bank, (offset >> 2) + k), k);
                }

                return;
            }

            Operand addr = context.IAdd(index, Const(offset));
            Operand word = context.ShiftRightU32(addr, Const(2));

            for (int k = 0; k < (size == 5 ? 2 : 1); k++)
            {
                Operand value = EmitLoadConstant(context, Const(bank), context.IAdd(word, Const(k)));
                ASet(context, x, size < 4 ? ExtractSmallInt(context, (LsSize)size, GetBitOffset(context, addr), value) : value, k);
            }
        }

        public static void AFswzadd(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            ASet(context, x, context.FPSwizzleAdd(AGpr(x.Ra), AGpr(x.Rc), x[32..40]));
        }

        // ---- integer ----

        // ISETP: Pd0 = (a cmp b) bop P, Pd1 = !(a cmp b) bop P (comparison 76..79, signed 73, bop 74..76). .EX (72): the high words of a 64-bit comparison whose
        // low words' result is the predicate at 68..71
        public static void AIsetp(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            IComp op = (IComp)x[76..79];
            bool signed = x[73];
            Operand a = ASrc(context, x, SA), b = ASrc(context, x, S32);
            Operand cmp = GetIntComparison(context, op, a, b, signed);

            if (x[72])
            {
                Operand low = APred(context, x, 68, 71), eq = context.BitwiseAnd(context.ICompareEqual(a, b), low);

                cmp = op switch
                {
                    IComp.Eq => eq,
                    IComp.Ne => context.BitwiseOr(context.ICompareNotEqual(a, b), low),
                    IComp.Lt or IComp.Le => context.BitwiseOr(GetIntComparison(context, IComp.Lt, a, b, signed), eq),
                    IComp.Gt or IComp.Ge => context.BitwiseOr(GetIntComparison(context, IComp.Gt, a, b, signed), eq),
                    _ => cmp,
                };
            }

            Operand p = APred(context, x, 87, 90);
            BoolOp bop = (BoolOp)x[74..76];

            ASetPred(context, x[81..84], GetPredLogicalOp(context, bop, cmp, p));
            ASetPred(context, x[84..87], GetPredLogicalOp(context, bop, context.BitwiseNot(cmp), p));
        }

        // IADD3: a + b + c, a negated source two's complement. .X (74): negation is bitwise not, and the carries 87..90 and 77..80 are added. The carries out of the
        // sum go to the predicates at 81..84 and 84..87
        public static void AIadd3(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool ex = x[74];
            Operand sum = null, carries = Const(0);
            int o0 = x[81..84], o1 = x[84..87];
            bool wantCarry = o0 != Pt || o1 != Pt;

            void Add(Operand v)
            {
                if (sum == null)
                {
                    sum = v;
                    return;
                }

                Operand next = context.IAdd(sum, v);

                if (wantCarry)
                {
                    carries = context.IAdd(carries, context.ConditionalSelect(context.ICompareLessUnsigned(next, sum), Const(1), Const(0)));
                }

                sum = next;
            }

            foreach (int slot in new[] { SA, SlotB(x), SlotC(x) })
            {
                Operand v = ASrc(context, x, slot);
                bool neg = AMods(x, slot).Neg;

                Add(neg ? (ex ? context.BitwiseNot(v) : context.INegate(v)) : v);
            }

            if (ex)
            {
                foreach ((int lo, int not) in new[] { (87, 90), (77, 80) })
                {
                    if (!(x[lo..(lo + 3)] == Pt && x[not]))
                    {
                        Add(context.ConditionalSelect(APred(context, x, lo, not), Const(1), Const(0)));
                    }
                }
            }

            ASet(context, x, sum);

            if (wantCarry)
            {
                ASetPred(context, o0, context.ICompareNotEqual(context.BitwiseAnd(carries, Const(1)), Const(0)));
                ASetPred(context, o1, context.ICompareNotEqual(context.BitwiseAnd(carries, Const(2)), Const(0)));
            }
        }

        // IMAD (0x024): the low word of a * b + c. IMAD.WIDE (0x025): the pair a * b + {c + 1:c}. IMAD.HI (0x027): the high word of that. Signed 73; .X (74) adds
        // the carry 87..90, a negated c then being its bitwise not; the carry out of a 64-bit addition goes to the predicate at 81..84
        public static void AImad(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int kind = x.Op & 0x1f;
            bool signed = x[73];
            Operand a = ASrc(context, x, SA), b = AI(context, x, SlotB(x));
            Operand c = ASrc(context, x, SlotC(x));
            bool cNeg = AMods(x, SlotC(x)).Neg;
            Operand carryIn = x[74] && !(x[87..90] == Pt && x[90]) ? context.ConditionalSelect(APred(context, x, 87, 90), Const(1), Const(0)) : null;

            Operand lo = context.IMultiply(a, b);

            if (kind == 0x04)
            {
                lo = context.IAdd(lo, cNeg ? (x[74] ? context.BitwiseNot(c) : context.INegate(c)) : c);
                ASet(context, x, carryIn != null ? context.IAdd(lo, carryIn) : lo);
                return;
            }

            if (cNeg)
            {
                throw AFail(context, "IMAD.WIDE / HI with a negated addend");
            }

            Operand hi = signed ? context.MultiplyHighS32(a, b) : context.MultiplyHighU32(a, b);
            (Operand sumLo, Operand carry) = AAddCarry(context, lo, c, carryIn);
            (Operand sumHi, Operand carryOut) = AAddCarry(context, hi, ASrcHigh(context, x, SlotC(x)), carry);

            ASetPred(context, x[81..84], context.ICompareNotEqual(carryOut, Const(0)));

            if (kind == 0x07)
            {
                ASet(context, x, sumHi);
                return;
            }

            ASet(context, x, sumLo);
            ASet(context, x, sumHi, 1);
        }

        // a + b (+ carry): the sum and its carry out (0 or 1)
        private static (Operand Sum, Operand Carry) AAddCarry(EmitterContext context, Operand a, Operand b, Operand carry)
        {
            Operand sum = context.IAdd(a, b);
            Operand c = context.ICompareLessUnsigned(sum, a);

            if (carry != null)
            {
                Operand next = context.IAdd(sum, carry);
                c = context.BitwiseOr(c, context.ICompareLessUnsigned(next, sum));
                sum = next;
            }

            return (sum, context.ConditionalSelect(c, Const(1), Const(0)));
        }

        // LOP3: lut 72..80; Pd (81..84) = (result != 0) OR P (87..90), AND with .PAND (80)
        public static void ALop3(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            Operand res = Lop3Expression.GetFromTruthTable(context, ASrc(context, x, SA), ASrc(context, x, SlotB(x)), ASrc(context, x, SlotC(x)), x[72..80]);
            ASet(context, x, res);

            if (x[81..84] != Pt)
            {
                Operand nz = context.ICompareNotEqual(res, Const(0)), p = APred(context, x, 87, 90);
                ASetPred(context, x[81..84], x[80] ? context.BitwiseAnd(nz, p) : context.BitwiseOr(nz, p));
            }
        }

        // SHF Rd, lo, shift, hi: a funnel shift of {hi:lo} (type 73..75: I64, U64, I32, U32; .W 75 wraps the shift, else clamps it; .R 76; .HI 80 the high word)
        public static void AShf(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int type = x[73..75];
            bool signed = type is 0 or 2, right = x[76], high = x[80];
            int width = type <= 1 ? 64 : 32;
            Operand lo = ASrc(context, x, SA), hi = ASrc(context, x, SlotC(x));
            Operand sh = ASrc(context, x, SlotB(x));

            sh = x[75] ? context.BitwiseAnd(sh, Const(width - 1)) : context.IMinimumU32(sh, Const(width));

            // The shift of {hi:lo}, word by word: below 32 the words mix, from 32 on one word moves into the other
            Operand under = context.ICompareLessUnsigned(sh, Const(32));
            Operand zero = context.ICompareEqual(sh, Const(0));
            Operand over = context.ISubtract(sh, Const(32));
            Operand back = context.ISubtract(Const(32), sh);
            Operand ShrHi(Operand n) => signed ? context.ShiftRightS32(hi, context.IMinimumU32(n, Const(31))) : context.ConditionalSelect(context.ICompareLessUnsigned(n, Const(32)), context.ShiftRightU32(hi, n), Const(0));
            Operand Shl(Operand v, Operand n) => context.ConditionalSelect(context.ICompareLessUnsigned(n, Const(32)), context.ShiftLeft(v, n), Const(0));

            Operand res = (right, high) switch
            {
                (false, false) => Shl(lo, sh),
                (true, true) => context.ConditionalSelect(under, ShrHi(sh), signed ? context.ShiftRightS32(hi, Const(31)) : Const(0)),
                (false, true) => context.ConditionalSelect(under,
                    context.ConditionalSelect(zero, hi, context.BitwiseOr(context.ShiftLeft(hi, sh), context.ShiftRightU32(lo, back))), Shl(lo, over)),
                (true, false) => context.ConditionalSelect(under,
                    context.ConditionalSelect(zero, lo, context.BitwiseOr(context.ShiftRightU32(lo, sh), context.ShiftLeft(hi, back))), ShrHi(over)),
            };

            ASet(context, x, res);
        }

        // LEA Rd, Pd, a, b, a_hi: (a << shift) + b (shift 75..80), with .HI (80) the high word of ({a_hi:a} << shift) + b. The shifted value is negated (72; bitwise
        // not with .X); .X (74) adds the carry 87..90; Pd (81..84) is the carry out
        public static void ALea(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int shift = x[75..80];
            bool ex = x[74];
            Operand lo = ASrc(context, x, SA), b = ASrc(context, x, SlotB(x));

            if (x.Form is 1 or 5 or 6 && x[63])
            {
                b = context.INegate(b);
            }

            Operand t = !x[80] ? context.ShiftLeft(lo, Const(shift))
                : shift == 0 ? ASrc(context, x, SlotC(x))
                : context.BitwiseOr(context.ShiftLeft(ASrc(context, x, SlotC(x)), Const(shift)), context.ShiftRightU32(lo, Const(32 - shift)));

            Operand carryIn = null;

            if (x[72])
            {
                t = context.BitwiseNot(t);
                carryIn = ex ? null : Const(1);
            }

            if (ex && !(x[87..90] == Pt && x[90]))
            {
                carryIn = context.ConditionalSelect(APred(context, x, 87, 90), Const(1), Const(0));
            }

            (Operand sum, Operand carry) = AAddCarry(context, t, b, carryIn);

            ASet(context, x, sum);
            ASetPred(context, x[81..84], context.ICompareNotEqual(carry, Const(0)));
        }

        // IMNMX: the minimum where P (87..90) holds; signed 73
        public static void AImnmx(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            Operand a = ASrc(context, x, SA), b = ASrc(context, x, S32);
            bool s = x[73];
            Operand min = s ? context.IMinimumS32(a, b) : context.IMinimumU32(a, b), max = s ? context.IMaximumS32(a, b) : context.IMaximumU32(a, b);
            ASet(context, x, context.ConditionalSelect(APred(context, x, 87, 90), min, max));
        }

        public static void AIabs(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            ASet(context, x, context.IAbsolute(ASrc(context, x, S32)));
        }

        // PRMT Rd, a, sel, c: byte i of the result is byte (s & 7) of {c:a}, or its sign repeated where s & 8; s = sel's nibble i (mode 72..75: index only)
        public static void APrmt(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x[72..75] != 0)
            {
                throw AFail(context, $"PRMT mode {x[72..75]}");
            }

            Operand a = ASrc(context, x, SA), sel = ASrc(context, x, SlotB(x)), c = ASrc(context, x, SlotC(x));
            Operand res = Const(0);

            for (int i = 0; i < 4; i++)
            {
                Operand s = sel.Type == OperandType.Constant ? Const((sel.Value >> (i * 4)) & 0xf) : context.BitfieldExtractU32(sel, Const(i * 4), Const(4));
                Operand src = context.ConditionalSelect(context.ICompareNotEqual(context.BitwiseAnd(s, Const(4)), Const(0)), c, a);
                Operand b = context.BitfieldExtractU32(src, context.ShiftLeft(context.BitwiseAnd(s, Const(3)), Const(3)), Const(8));
                Operand sign = context.ShiftRightS32(context.ShiftLeft(b, Const(24)), Const(31));

                b = context.ConditionalSelect(context.ICompareNotEqual(context.BitwiseAnd(s, Const(8)), Const(0)), sign, b);
                res = context.BitwiseOr(res, context.ShiftLeft(context.BitwiseAnd(b, Const(0xff)), Const(i * 8)));
            }

            ASet(context, x, res);
        }

        // POPC / FLO / BREV of the 32-slot operand, bitwise-inverted with 63. FLO: signed 73, .SH (74) as a shift amount
        public static void ABits(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            Operand src = ASrc(context, x, S32);

            if (x[63])
            {
                src = context.BitwiseNot(src);
            }

            Operand res = (x.Op & 0x7f) switch
            {
                0x09 or 0x3f => context.BitCount(src),
                0x01 or 0x3e => context.BitfieldReverse(src),
                _ => x[73] ? context.FindMSBS32(src) : context.FindMSBU32(src),
            };

            if ((x.Op & 0x7f) is 0x00 or 0x3d && x[74])
            {
                res = context.ConditionalSelect(context.ICompareEqual(res, Const(-1)), res, context.BitwiseExclusiveOr(res, Const(31)));
            }

            ASet(context, x, res);
        }

        // SGXT Rd, a, n: the low n bits of a, sign- (73) or zero-extended; .W (75) wraps n, else it is clamped to 32
        public static void ASgxt(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            Operand n = ASrc(context, x, S32), a = ASrc(context, x, SA);

            n = x[75] ? context.BitwiseAnd(n, Const(31)) : context.IMinimumU32(n, Const(32));
            Operand res = x[73] ? context.BitfieldExtractS32(a, Const(0), n) : context.BitfieldExtractU32(a, Const(0), n);
            ASet(context, x, context.ConditionalSelect(context.ICompareEqual(n, Const(0)), Const(0), res));
        }

        // ---- predicates ----

        // R2P PR, Ra, mask: predicates from the byte 76..78 of Ra, the mask an immediate (0x804) or a register (0x204)
        public static void AR2p(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            EmitR2p(context, AGpr(x.Ra), x.Op == 0x804 ? Const(x.Imm32) : AGpr(x.Rb), (ByteSel)x[76..78], false);
        }

        // P2R Rd, PR, Ra, mask: the predicates into the byte 76..78 of Ra
        public static void AP2r(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            EmitP2r(context, AGpr(x.Ra), Register(x.Rd, RegisterType.Gpr), x.Op == 0x803 ? Const(x.Imm32) : AGpr(x.Rb), (ByteSel)x[76..78], false);
        }

        // PLOP3 Pd0, Pd1, a (87..90), b (77..80), c (68..71; a uniform one with 67): Pd0 by the truth table 64..67 | 72..77 << 3, Pd1 by 16..24
        public static void APlop3(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool u = IsUniform(context);
            Operand a = APred(context, x, 87, 90), b = APred(context, x, 77, 80), c = APred(context, x, 68, 71, u || x[67]);

            ASetPred(context, x[81..84], APredLut(context, a, b, c, x[64..67] | (x[72..77] << 3)));
            ASetPred(context, x[84..87], APredLut(context, a, b, c, x[16..24]));
        }

        // A three-input boolean function by its truth table, as LOP3's (a = 0xf0, b = 0xcc, c = 0xaa)
        private static Operand APredLut(EmitterContext context, Operand a, Operand b, Operand c, int lut)
        {
            Operand res = Const(IrConsts.False);

            for (int m = 0; m < 8; m++)
            {
                if ((lut >> m & 1) != 0)
                {
                    Operand term = context.BitwiseAnd(context.BitwiseAnd((m & 4) != 0 ? a : context.BitwiseNot(a), (m & 2) != 0 ? b : context.BitwiseNot(b)),
                        (m & 1) != 0 ? c : context.BitwiseNot(c));
                    res = context.BitwiseOr(res, term);
                }
            }

            return res;
        }

        // VOTE / VOTEU Rd, Pd, P: Pd (81..84) the vote (72..74: all, any, equal) of P (87..90); Rd (a uniform register for VOTEU) the ballot
        public static void AVote(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            Operand pred = APred(context, x, 87, 90, false);
            Operand vote = EmitVote(context, (VoteMode)x[72..74], pred);

            if (vote != null && x[81..84] != Pt)
            {
                context.Copy(Register(x[81..84], RegisterType.Predicate), vote);
            }

            ASet(context, x.Op == 0x886 ? Register(x[16..22], RegisterType.Uniform) : Register(x.Rd, RegisterType.Gpr), EmitBallot(context, pred));
        }

        // SHFL Rd, Pd, a, lane, c: lane a register (32..40) or an immediate (53..58; 0x989, 0xf89), c a register (64..72) or an immediate (40..53; 0x589, 0xf89),
        // mode 58..60
        public static void AShfl(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            Operand a = AGpr(x.Ra);
            Operand lane = x.Op is 0x989 or 0xf89 ? Const(x[53..58]) : AGpr(x.Rb);
            Operand c = x.Op is 0x589 or 0xf89 ? Const(x[40..53]) : AGpr(x.Rc);

            (Operand res, Operand valid) = (ShflMode)x[58..60] switch
            {
                ShflMode.Idx => context.Shuffle(a, lane, c),
                ShflMode.Up => context.ShuffleUp(a, lane, c),
                ShflMode.Down => context.ShuffleDown(a, lane, c),
                _ => context.ShuffleXor(a, lane, c),
            };

            ASet(context, x, res);
            ASetPred(context, x[81..84], valid);
        }
    }
}
