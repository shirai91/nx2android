using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.Linq;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Instructions
{
    [Flags]
    enum AmpereProps
    {
        None = 0,
        Pure = 1 << 0,      // no side effect: a guarded instance runs unconditionally and selects its results
        NoRd = 1 << 1,      // bits 16..24 are not a destination register
        Uniform = 1 << 2,   // the uniform datapath: UR / UP operands, a UP guard
        Branch = 1 << 3,    // ends its block; takes care of its own guard
    }

    // Ampere opcode → emitter. ALU instructions are registered by their 9-bit opcode (the operand form is bits 9..11), the others by all 12 bits, which win.
    static class AmpereTable
    {
        public const int Bra = 0x947, Brx = 0x949, Brxu = 0x958, Exit = 0x94d, Kill = 0x95b, Ret = 0x950, Call = 0x944, Depbar = 0x91a;
        public const int Ipa = 0x326, IpaIndexed = 0x526, Ald = 0x321, Ast = 0x322, Ldc = 0xb82, Uldc = 0xab9, UldcIndexed = 0xabb;

        private readonly record struct Entry(InstName Name, InstEmitter Emitter, AmpereProps Props, FeatureFlags Features);

        private static readonly Dictionary<int, Entry> _exact = new(), _alu = new();

        static AmpereTable()
        {
            const AmpereProps P = AmpereProps.Pure, N = AmpereProps.NoRd, B = AmpereProps.Branch | AmpereProps.NoRd;

            X(Bra, InstName.Bra, InstEmit.ABra, B);
            X(Brx, InstName.Brx, InstEmit.ABrx, B);
            X(Brxu, InstName.Brx, InstEmit.ABrx, B);
            X(Exit, InstName.Exit, InstEmit.AExit, B);
            X(Kill, InstName.Kil, InstEmit.AKill, B);
            X(Ret, InstName.Ret, InstEmit.ARet, B);
            X(Call, InstName.Cal, InstEmit.ACall, B);

            // NOP, DEPBAR, convergence barriers (BSSY, BSYNC, BREAK), WARPSYNC and an EXIT's continuation setup (0x357, 0x957) only order a warp's threads
            foreach (int op in new[] { 0x918, Depbar, 0x945, 0x941, 0x942, 0x948, 0x357, 0x957 })
            {
                X(op, InstName.Nop, _ => { }, P | N);
            }

            InstEmit.RegisterAmpereAlu(Alu, X);
            InstEmit.RegisterAmpereMemory(X);

            void X(int op, InstName name, InstEmitter emitter, AmpereProps props, FeatureFlags features = FeatureFlags.None) =>
                _exact[op] = new(name, emitter, props, features);

            void Alu(int op, InstName name, InstEmitter emitter, AmpereProps props) => _alu[op] = new(name, emitter, props | P, FeatureFlags.None);
        }

        private static bool TryGet(int op, out Entry entry) => _exact.TryGetValue(op, out entry) || _alu.TryGetValue(op & 0x1ff, out entry);

        public static InstOp GetOp(ulong address, ulong lo, ulong hi)
        {
            int op = (int)(lo & 0xfff);

            return TryGet(op, out Entry e)
                ? new InstOp(address, lo, hi, e.Name, e.Emitter, e.Props.HasFlag(AmpereProps.Branch) ? InstProps.Bra : InstProps.None)
                : new InstOp(address, lo, hi, InstName.Invalid, InstEmit.AUnsupported, InstProps.None);
        }

        public static InstOp AsJump(InstOp op) => new(op.Address, op.RawOpCode, op.RawOpCodeHi, InstName.Bra, InstEmit.ABra, InstProps.Bra);

        public static AmpereProps Props(int op) => TryGet(op, out Entry e) ? e.Props : AmpereProps.None;

        public static bool WritesRd(int op) => !Props(op).HasFlag(AmpereProps.NoRd);

        public static bool IsUniform(int op) => Props(op).HasFlag(AmpereProps.Uniform);

        public static FeatureFlags Features(int op) => TryGet(op, out Entry e) ? e.Features : FeatureFlags.None;
    }

    // Results of variable-latency instructions held in shadow registers until an instruction waits on their scoreboard: the compiler lets the instructions
    // in between read the registers' old values (LDG R2, [R2.64] then LDG R3, [R2.64 + 4] reads the address twice)
    sealed class AmpereScoreboards
    {
        private readonly List<(Register Reg, Operand Shadow, Operand Guard)>[] _pending = new List<(Register, Operand, Operand)>[6];
        private readonly Stack<int> _free = new(Enumerable.Range(0, RegisterConsts.ShadowsCount).Reverse());

        public Operand NewShadow() => Register(_free.Pop(), RegisterType.Shadow);

        public void Free(Operand shadow) => _free.Push(shadow.GetRegister().Index);

        public void Hold(int barrier, Register reg, Operand shadow, Operand guard) => (_pending[barrier] ??= []).Add((reg, shadow, guard));

        public void Commit(EmitterContext context, int barrier)
        {
            foreach ((Register reg, Operand shadow, Operand guard) in _pending[barrier] ?? [])
            {
                InstEmit.AWrite(context, Register(reg), shadow, guard);
                Free(shadow);
            }

            _pending[barrier] = null;
        }

        public void CommitAll(EmitterContext context)
        {
            for (int b = 0; b < _pending.Length; b++)
            {
                Commit(context, b);
            }
        }

        // A register written again before its held result landed: the newer write stands
        public void Drop(Register reg)
        {
            foreach (List<(Register Reg, Operand Shadow, Operand Guard)> list in _pending)
            {
                foreach ((Register r, Operand shadow, _) in list?.FindAll(p => p.Reg.Equals(reg)) ?? [])
                {
                    Free(shadow);
                }

                list?.RemoveAll(p => p.Reg.Equals(reg));
            }
        }
    }

    static partial class InstEmit
    {
        private const int Rz = RegisterConsts.RegisterZeroIndex, Pt = RegisterConsts.PredicateTrueIndex;

        public static void EmitAmpereBlock(EmitterContext context, Block block)
        {
            AmpereScoreboards sb = context.Scoreboards ??= new AmpereScoreboards();

            for (int i = 0; i < block.OpCodes.Count; i++)
            {
                InstOp op = block.OpCodes[i];
                AmpereOp x = new(op);
                AmpereProps props = AmpereTable.Props(x.Op);

                for (int b = 0; b < 6; b++)
                {
                    if ((x.WaitMask & (1 << b)) != 0)
                    {
                        sb.Commit(context, b);
                    }
                }

                if (x.Op == AmpereTable.Depbar || op.Props.HasFlag(InstProps.Bra))
                {
                    sb.CommitAll(context);
                }

                if (context.TranslatorContext.Options.Flags.HasFlag(TranslationFlags.DebugMode))
                {
                    context.Add(new CommentNode($"0x{op.Address:X6}: {x.Hi:X16}{x.Lo:X16} {op.Name}"));
                }

                if (x.Guard == Pt && x.GuardNot)
                {
                    continue;
                }

                context.CurrOp = op;

                Operand guard = x.Guard == Pt || op.Name == InstName.Bra ? null : APred(context, x, 12, 15, props.HasFlag(AmpereProps.Uniform));
                bool select = guard != null && props.HasFlag(AmpereProps.Pure);
                bool hold = x.WriteBarrier < 6 && !op.Props.HasFlag(InstProps.Bra);
                int start = context.OperationsCount;
                Operand skip = null;

                if (guard != null && !select)
                {
                    bool last = i == block.OpCodes.Count - 1 && block.HasNext();
                    skip = last ? context.GetLabel(block.Successors[0].Address) : Label();
                    context.BranchIfFalse(skip, guard);
                    skip = last ? null : skip;
                }

                op.Emitter(context);

                if (skip != null)
                {
                    context.MarkLabel(skip);
                }

                if (select || hold)
                {
                    Operand g = select && hold ? context.Copy(guard) : guard;

                    foreach ((Register reg, Operand shadow) in Redirect(context, start, sb))
                    {
                        if (hold)
                        {
                            sb.Hold(x.WriteBarrier, reg, shadow, select ? g : null);
                        }
                        else
                        {
                            AWrite(context, Register(reg), shadow, g);
                            sb.Free(shadow);
                            sb.Drop(reg);
                        }
                    }
                }
                else
                {
                    foreach (Register reg in Dests(context, start))
                    {
                        sb.Drop(reg);
                    }
                }
            }

            sb.CommitAll(context);
        }

        // reg = shadow, or where guard holds
        public static void AWrite(EmitterContext context, Operand reg, Operand shadow, Operand guard)
        {
            if (guard == null)
            {
                context.Copy(reg, shadow);
            }
            else if (reg.GetRegister().Type is RegisterType.Predicate or RegisterType.UniformPredicate)
            {
                context.Copy(reg, context.BitwiseOr(context.BitwiseAnd(guard, shadow), context.BitwiseAnd(context.BitwiseNot(guard), reg)));
            }
            else
            {
                context.Copy(reg, context.ConditionalSelect(guard, shadow, reg));
            }
        }

        private static IEnumerable<Register> Dests(EmitterContext context, int start)
        {
            for (int i = start; i < context.OperationsCount; i++)
            {
                Operation operation = context.GetOperation(i);

                for (int d = 0; d < operation.DestsCount; d++)
                {
                    if (operation.GetDest(d) is { Type: OperandType.Register } dest && dest.GetRegister().Type != RegisterType.Shadow)
                    {
                        yield return dest.GetRegister();
                    }
                }
            }
        }

        // The registers the operations from start on write become shadow registers there (reads included), each first set to the register's value
        private static List<(Register, Operand)> Redirect(EmitterContext context, int start, AmpereScoreboards sb)
        {
            Dictionary<Register, Operand> shadows = new();

            foreach (Register reg in Dests(context, start).Distinct())
            {
                shadows[reg] = sb.NewShadow();
            }

            for (int i = start; i < context.OperationsCount; i++)
            {
                Operation operation = context.GetOperation(i);

                for (int d = 0; d < operation.DestsCount; d++)
                {
                    if (operation.GetDest(d) is { Type: OperandType.Register } dest && shadows.TryGetValue(dest.GetRegister(), out Operand shadow))
                    {
                        operation.SetDest(d, shadow);
                    }
                }

                for (int s = 0; s < operation.SourcesCount; s++)
                {
                    if (operation.GetSource(s) is { Type: OperandType.Register } src && shadows.TryGetValue(src.GetRegister(), out Operand shadow))
                    {
                        operation.SetSource(s, shadow);
                    }
                }
            }

            foreach ((Register reg, Operand shadow) in shadows)
            {
                context.InsertOperation(start, new Operation(Instruction.Copy, shadow, Register(reg)));
            }

            return shadows.Select(p => (p.Key, p.Value)).ToList();
        }

        public static void AUnsupported(EmitterContext context) => throw AFail(context, "not implemented");

        private static NotSupportedException AFail(EmitterContext context, string what)
        {
            AmpereOp x = new(context.CurrOp);

            return new NotSupportedException($"Ampere {x.Op:x3} at 0x{context.CurrOp.Address:x} ({x.Hi:x16}{x.Lo:x16}): {what}");
        }

        // ---- operands ----

        // ALU operand slots: A (24), the 32 slot (a register, an immediate, a constant or a uniform register, by the form) and the 64 slot (a register).
        // Three-source operations take B, C from (32, 64) in forms 1, 4, 5, 6 and from (64, 32) in forms 2, 3, 7; two-source ones A and the 32 slot.
        private const int SA = 0, S32 = 1, S64 = 2;

        private static int SlotB(AmpereOp x) => x.Form is 2 or 3 or 7 ? S64 : S32;

        private static int SlotC(AmpereOp x) => x.Form is 2 or 3 or 7 ? S32 : S64;

        private static bool IsUniform(EmitterContext context) => AmpereTable.IsUniform(new AmpereOp(context.CurrOp).Op);

        private static Operand ASrc(EmitterContext context, AmpereOp x, int slot)
        {
            bool u = IsUniform(context);

            if (slot == S32)
            {
                switch (x.Form)
                {
                    case 2 or 4:
                        return Const(x.Imm32);
                    case 3 or 5:
                        return ACbuf(context, x);
                    case 6 or 7:
                        return Register(x[32..38], RegisterType.Uniform);
                }
            }

            int at = slot switch { SA => 24, S32 => 32, _ => 64 };

            return u ? Register(x[at..(at + 6)], RegisterType.Uniform) : Register(x[at..(at + 8)], RegisterType.Gpr);
        }

        // The upper word of a 64-bit operand: the next register, the next constant word, or an immediate's sign
        private static Operand ASrcHigh(EmitterContext context, AmpereOp x, int slot)
        {
            Operand lo = ASrc(context, x, slot);

            return lo.Type switch
            {
                OperandType.Register => Register(lo.GetRegister().Index + 1, lo.GetRegister().Type),
                OperandType.ConstantBuffer => Cbuf(lo.GetCbufSlot(), lo.GetCbufOffset() + 1),
                _ => Const(lo.Value < 0 && x.Form is 2 or 4 ? -1 : 0),
            };
        }

        // Negation and absolute value bits; an immediate takes all of 32..64
        private static (bool Neg, bool Abs) AMods(AmpereOp x, int slot) => slot switch
        {
            SA => (x[72], x[73]),
            S32 => x.Form is 2 or 4 ? (false, false) : (x[63], x[62]),
            _ => (x[75], x[74]),
        };

        private static Operand AF(EmitterContext context, AmpereOp x, int slot, Instruction type = Instruction.FP32)
        {
            (bool neg, bool abs) = AMods(x, slot);

            return context.FPAbsNeg(ASrc(context, x, slot), abs, neg, type);
        }

        private static Operand AI(EmitterContext context, AmpereOp x, int slot)
        {
            Operand v = ASrc(context, x, slot);

            return AMods(x, slot).Neg ? context.INegate(v) : v;
        }

        // A constant operand c[bank][offset] (bank 54..59, byte offset 38..54); 91 = the bank in a uniform register, not met
        private static Operand ACbuf(EmitterContext context, AmpereOp x)
        {
            if (x[91])
            {
                throw AFail(context, "constant operand with its bank in a uniform register");
            }

            (int bank, int offset) = AMapC0(context, x[54..59], x[38..54]);

            return Cbuf(bank, offset >> 2);
        }

        // NVN2 compute shaders keep their driver constants in c[0] in a layout of their own, the counts in the control section: from 0x10 the push constants, then
        // 0x10 bytes per uniform buffer slot, per storage buffer slot, then 8 bytes per texture handle and per image handle. They are moved to where NVN's layout
        // has them, which the reflection and the host read: push constants to c[24], uniform slot r to c[0] + 0x210 + 0x10 r, storage slot s to c[0] + 0x310 + 0x10 s,
        // texture handle t to c[2] + 0x20 + 8 t, image handle i to c[2] + 0x120 + 8 i
        private static (int Bank, int Offset) AMapC0(EmitterContext context, int bank, int offset)
        {
            int[] l = bank == 0 ? context.TranslatorContext.GpuAccessor.QueryAmpereComputeC0Layout() : null;

            if (l == null)
            {
                return (bank, offset);
            }

            int push = 0x10 + ((l[4] + 0xf) & ~0xf), ubo = push + 0x10 * l[0], sbo = ubo + 0x10 * l[1], tex = sbo + 8 * l[2];

            return offset switch
            {
                >= 0x10 when offset < push => (24, offset - 0x10),
                _ when offset >= push && offset < ubo => (0, 0x210 + offset - push),
                _ when offset >= ubo && offset < sbo => (0, 0x310 + offset - ubo),
                _ when offset >= sbo && offset < tex => (2, 0x20 + offset - sbo),
                _ when offset >= tex && offset < l[3] => (2, 0x120 + offset - tex),
                _ => (bank, offset),
            };
        }

        // The destination: Rd (16..24), or URd (16..22) on the uniform datapath
        private static Operand ADst(EmitterContext context, AmpereOp x, int k = 0)
        {
            return IsUniform(context) ? Register(x[16..22] + k, RegisterType.Uniform) : Register(x.Rd + k, RegisterType.Gpr);
        }

        private static void ASet(EmitterContext context, AmpereOp x, Operand value, int k = 0)
        {
            ASet(context, ADst(context, x, k), value);
        }

        private static void ASet(EmitterContext context, Operand dest, Operand value)
        {
            if (dest.Type == OperandType.Register)
            {
                context.Copy(dest, value);
            }
        }

        private static Operand AGpr(int reg, int k = 0) => reg == Rz ? Const(0) : Register(reg + k, RegisterType.Gpr);

        // A predicate source: 3 bits at lo, inverted by notBit
        private static Operand APred(EmitterContext context, AmpereOp x, int lo, int notBit, bool uniform)
        {
            Operand p = Register(x[lo..(lo + 3)], uniform ? RegisterType.UniformPredicate : RegisterType.Predicate);

            return x[notBit] ? context.BitwiseNot(p) : p;
        }

        private static Operand APred(EmitterContext context, AmpereOp x, int lo, int notBit) => APred(context, x, lo, notBit, IsUniform(context));

        private static void ASetPred(EmitterContext context, int index, Operand value)
        {
            if (index != Pt)
            {
                context.Copy(Register(index, IsUniform(context) ? RegisterType.UniformPredicate : RegisterType.Predicate), value);
            }
        }

        // A predicate that is always true is left out
        private static Operand ACond(EmitterContext context, AmpereOp x, int lo, int notBit)
        {
            return x[lo..(lo + 3)] == Pt && !x[notBit] ? null : APred(context, x, lo, notBit, false);
        }

        // ---- control flow ----

        // BRA: taken where the guard and the condition (87..90) hold. Also a CALL into code that never returns
        public static void ABra(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            Operand label = context.GetLabel(Decoder.AmpereTarget(context.CurrOp));
            Operand cond = x.Guard == Pt && !x.GuardNot ? null : APred(context, x, 12, 15, false);
            Operand c = x.Op == AmpereTable.Bra ? ACond(context, x, 87, 90) : null;

            cond = cond == null ? c : c == null ? cond : context.BitwiseAnd(cond, c);

            if (cond == null)
            {
                context.Branch(label);
            }
            else
            {
                context.BranchIfTrue(label, cond);
            }
        }

        // BRX Ra, imm (BRXU: a uniform register): to the next instruction + imm + Ra; the decoder took the targets from the jump table. Every target but the last
        // is taken where the address matches
        public static void ABrx(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            Block[] targets = context.CurrBlock.Successors.Skip(context.CurrBlock.HasNext() ? 1 : 0).OrderBy(b => b.Address).ToArray();

            if (targets.Length == 0)
            {
                throw AFail(context, "BRX without a target");
            }

            Operand reg = Register(x.Ra, x.Op == AmpereTable.Brxu ? RegisterType.Uniform : RegisterType.Gpr);
            Operand address = context.IAdd(reg, Const((int)Decoder.AmpereTarget(context.CurrOp)));

            for (int i = 0; i < targets.Length - 1; i++)
            {
                context.BranchIfTrue(context.GetLabel(targets[i].Address), context.ICompareEqual(address, Const((int)targets[i].Address)));
            }

            context.Branch(context.GetLabel(targets[^1].Address));
        }

        public static void AExit(EmitterContext context)
        {
            Operand skip = ACondSkip(context);

            if (context.IsNonMain)
            {
                throw AFail(context, "EXIT in a called function");
            }

            if (context.PrepareForReturn())
            {
                context.Return();
            }

            AEndSkip(context, skip);
        }

        public static void AKill(EmitterContext context)
        {
            Operand skip = ACondSkip(context);
            context.Discard();
            AEndSkip(context, skip);
        }

        // EXIT and KILL's condition (87..90): skipped where it does not hold
        private static Operand ACondSkip(EmitterContext context)
        {
            Operand cond = ACond(context, new AmpereOp(context.CurrOp), 87, 90);

            if (cond == null)
            {
                return null;
            }

            Operand skip = Label();
            context.BranchIfFalse(skip, cond);

            return skip;
        }

        private static void AEndSkip(EmitterContext context, Operand skip)
        {
            if (skip != null)
            {
                context.MarkLabel(skip);
            }
        }

        public static void ACall(EmitterContext context)
        {
            context.Call(context.Program.GetFunctionByAddress(Decoder.AmpereTarget(context.CurrOp)).Id, false);
        }

        public static void ARet(EmitterContext context)
        {
            if (!context.IsNonMain)
            {
                throw AFail(context, "RET in the main function");
            }

            context.Return();
        }
    }
}
