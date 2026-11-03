using Ryujinx.Graphics.Shader.Instructions;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.Linq;

namespace Ryujinx.Graphics.Shader.Decoders
{
    // Ampere (sm_86) code as NVN2 ships it: 128-bit instructions, no scheduling words, convergence barriers (BSSY / BSYNC) instead of a sync stack, and CALL / RET.
    // The code area holds instructions only, so it is decoded whole first; then each function's instructions are walked from its entry. A CALL whose target
    // never reaches a RET (the compiler's jump into a shared EXIT tail) is a branch. BRX targets come from a jump table in c1.
    static partial class Decoder
    {
        public const int AmpereInstSize = 16;

        public static DecodedProgram DecodeAmpere(ShaderDefinitions definitions, IGpuAccessor gpuAccessor, ulong startAddress)
        {
            ReadOnlySpan<ulong> words = gpuAccessor.GetCode(startAddress, AmpereInstSize);
            InstOp[] ops = new InstOp[words.Length / 2];

            for (int i = 0; i < ops.Length; i++)
            {
                ops[i] = AmpereTable.GetOp((ulong)i * AmpereInstSize, words[2 * i], words[2 * i + 1]);
            }

            Context context = new(gpuAccessor);
            AmpereCode code = new(ops, context, gpuAccessor);
            HashSet<ulong> jumps = [];

            while (true)
            {
                Dictionary<ulong, DecodedFunction> functions = new();
                Queue<DecodedFunction> queue = new();

                DecodedFunction Enqueue(ulong address)
                {
                    if (!functions.TryGetValue(address, out DecodedFunction function))
                    {
                        functions.Add(address, function = new DecodedFunction(address));
                        queue.Enqueue(function);
                    }

                    return function;
                }

                DecodedFunction main = Enqueue(0);

                while (queue.TryDequeue(out DecodedFunction function))
                {
                    function.SetBlocks(code.Blocks(function.Address, jumps, callee => Enqueue(callee).AddCaller(function)));
                }

                bool changed = false;

                foreach (DecodedFunction function in functions.Values)
                {
                    if (function != main && !function.Blocks.Any(b => b.OpCodes.Count != 0 && new AmpereOp(b.GetLastOp()).Op == AmpereTable.Ret))
                    {
                        changed |= jumps.Add(function.Address);
                    }
                }

                if (!changed)
                {
                    foreach (InstOp op in functions.Values.SelectMany(f => f.Blocks).SelectMany(b => b.OpCodes))
                    {
                        NoteAmpereUse(definitions, context, new AmpereOp(op));
                    }

                    return new DecodedProgram(main, functions, context.AttributeUsage, context.UsedFeatures, context.ClipDistancesWritten, context.Cb1DataSize);
                }
            }
        }

        public static ulong AmpereTarget(InstOp op)
        {
            return (ulong)((long)op.Address + AmpereInstSize + new AmpereOp(op).BranchOffset);
        }

        // Whether execution never goes on at the next instruction: an unguarded BRA, BRX, EXIT, KILL, RET or jumping CALL. BRA, EXIT and KILL also have a condition at 87..90.
        private static bool IsUnconditionalAmpere(InstOp op)
        {
            AmpereOp x = new(op);

            if (!op.Props.HasFlag(InstProps.Bra) || op.Name == InstName.Cal || x.Guard != RegisterConsts.PredicateTrueIndex || x.GuardNot)
            {
                return false;
            }

            return x.Op is not (AmpereTable.Bra or AmpereTable.Exit or AmpereTable.Kill) || (x[87..90] == RegisterConsts.PredicateTrueIndex && !x[90]);
        }

        private sealed class AmpereCode
        {
            private readonly InstOp[] _ops;
            private readonly Context _context;
            private readonly int _c1Size;
            private readonly SortedSet<int> _c1Loads = [];   // the c1 offsets the code loads at: a jump table ends where the next begins

            public AmpereCode(InstOp[] ops, Context context, IGpuAccessor gpuAccessor)
            {
                _ops = ops;
                _context = context;
                _c1Size = gpuAccessor.QueryConstantBuffer1Size();

                foreach (InstOp op in ops)
                {
                    if (C1Load(new AmpereOp(op)) is int offset)
                    {
                        _c1Loads.Add(offset);
                    }
                }
            }

            // LDC Rd, c[1][Ra + offset] or ULDC URd, c[1][(URa +) offset] (no bindless bank, no address mode): the offset
            private static int? C1Load(AmpereOp x)
            {
                bool bound = x.Op == AmpereTable.UldcIndexed || (x.Op is AmpereTable.Ldc or AmpereTable.Uldc && !x[91]);

                return bound && x[54..59] == 1 && x[78..80] == 0 ? (int)x.Signed(38..54) : null;
            }

            private int Index(ulong address) => (int)(address / AmpereInstSize);

            private ulong Address(int index) => (ulong)index * AmpereInstSize;

            public Block[] Blocks(ulong entry, HashSet<ulong> jumps, Action<ulong> call)
            {
                Dictionary<int, List<int>> preds = new();
                Dictionary<int, int[]> brx = new();
                HashSet<int> reached = [];
                SortedSet<int> leaders = [Index(entry)];
                Stack<int> work = new([Index(entry)]);

                void Edge(int from, int to, bool leader)
                {
                    if ((uint)to >= (uint)_ops.Length)
                    {
                        return;
                    }

                    if (!preds.TryGetValue(to, out List<int> list))
                    {
                        preds[to] = list = [];
                    }

                    list.Add(from);

                    if (leader)
                    {
                        leaders.Add(to);
                    }

                    if (reached.Add(to))
                    {
                        work.Push(to);
                    }
                }

                reached.Add(Index(entry));

                while (true)
                {
                    while (work.TryPop(out int i))
                    {
                        InstOp op = Resolve(_ops[i], jumps);

                        if (op.Name == InstName.Cal)
                        {
                            call(AmpereTarget(op));
                        }
                        else if (op.Name == InstName.Bra)
                        {
                            Edge(i, Index(AmpereTarget(op)), true);
                        }
                        else if (brx.TryGetValue(i, out int[] targets))
                        {
                            foreach (int t in targets)
                            {
                                Edge(i, t, true);
                            }
                        }

                        if (!IsUnconditionalAmpere(op))
                        {
                            Edge(i, i + 1, op.Props.HasFlag(InstProps.Bra));
                        }
                    }

                    // BRX: once the code before it is known, its jump table
                    bool more = false;

                    foreach (int i in reached.Where(i => _ops[i].Name == InstName.Brx && !brx.ContainsKey(i)).ToArray())
                    {
                        brx[i] = BrxTargets(i, preds);
                        work.Push(i);
                        more = true;
                    }

                    if (!more)
                    {
                        break;
                    }
                }

                return MakeBlocks(reached, leaders, jumps, brx);
            }

            // BRX Ra, imm: to the next instruction + imm + Ra, Ra loaded from c1 by an LDC (ULDC for BRXU) on every path to the BRX. The table runs from that load's
            // offset to the next offset the code loads c1 at; it ends early at an entry that is not an instruction of the code. An entry for the program start is
            // padding (the unused cases of a table), not a target.
            private int[] BrxTargets(int brxIndex, Dictionary<int, List<int>> preds)
            {
                AmpereOp brx = new(_ops[brxIndex]);
                bool uniform = brx.Op == AmpereTable.Brxu;
                int reg = brx.Ra;
                int? table = null;
                HashSet<int> seen = [];
                Stack<int> work = new(preds.GetValueOrDefault(brxIndex) ?? []);

                while (work.TryPop(out int i))
                {
                    if (!seen.Add(i))
                    {
                        continue;
                    }

                    AmpereOp x = new(_ops[i]);

                    if (AmpereTable.WritesRd(x.Op) && AmpereTable.IsUniform(x.Op) == uniform && (uniform ? x[16..22] : x.Rd) == reg)
                    {
                        int? offset = C1Load(x);

                        if (offset == null || (table != null && table != offset))
                        {
                            throw new InvalidOperationException($"BRX at 0x{_ops[brxIndex].Address:x}: its jump table register is not loaded from one c1 offset");
                        }

                        table = offset;
                        continue;
                    }

                    foreach (int p in preds.GetValueOrDefault(i) ?? [])
                    {
                        work.Push(p);
                    }
                }

                if (table is not int start)
                {
                    throw new InvalidOperationException($"BRX at 0x{_ops[brxIndex].Address:x}: no c1 load of its jump table register");
                }

                int end = _c1Loads.GetViewBetween(start + 1, int.MaxValue).FirstOrDefault(_c1Size);
                long next = (long)Address(brxIndex) + AmpereInstSize + brx.BranchOffset;
                List<int> targets = [];

                for (int offset = start; offset + 4 <= Math.Min(end, _c1Size); offset += 4)
                {
                    long target = next + (int)_context.ConstantBuffer1Read(offset);

                    if (target == 0)
                    {
                        continue;
                    }

                    if (target < 0 || target % AmpereInstSize != 0 || target / AmpereInstSize >= _ops.Length)
                    {
                        break;
                    }

                    targets.Add((int)(target / AmpereInstSize));
                }

                return targets.Distinct().ToArray();
            }

            // A CALL into code that never returns is a jump
            private static InstOp Resolve(InstOp op, HashSet<ulong> jumps)
            {
                return op.Name == InstName.Cal && jumps.Contains(AmpereTarget(op)) ? AmpereTable.AsJump(op) : op;
            }

            private Block[] MakeBlocks(HashSet<int> reached, SortedSet<int> leaders, HashSet<ulong> jumps, Dictionary<int, int[]> brx)
            {
                Dictionary<int, Block> blocks = leaders.Where(reached.Contains).ToDictionary(i => i, i => new Block(Address(i)));
                Block Get(int i) => blocks[i];

                foreach ((int start, Block block) in blocks)
                {
                    int i = start;

                    while (true)
                    {
                        InstOp op = Resolve(_ops[i], jumps);

                        block.OpCodes.Add(op);
                        i++;

                        if (op.Props.HasFlag(InstProps.Bra) || i >= _ops.Length || blocks.ContainsKey(i))
                        {
                            break;
                        }
                    }

                    block.EndAddress = Address(i);
                }

                foreach ((int start, Block block) in blocks)
                {
                    InstOp last = block.GetLastOp();
                    int end = (int)(block.EndAddress / AmpereInstSize);

                    if (block.HasNext() && blocks.ContainsKey(end))
                    {
                        block.Successors.Add(Get(end));
                    }

                    IEnumerable<int> targets = last.Name switch
                    {
                        InstName.Bra => [Index(AmpereTarget(last))],
                        InstName.Brx => brx[Index(last.Address)],
                        _ => [],
                    };

                    foreach (int t in targets.Where(blocks.ContainsKey))
                    {
                        if (!block.Successors.Contains(Get(t)))
                        {
                            block.Successors.Add(Get(t));
                        }
                    }

                    foreach (Block s in block.Successors)
                    {
                        s.Predecessors.Add(block);
                    }
                }

                return blocks.Values.OrderBy(b => b.Address).ToArray();
            }
        }

        private static void NoteAmpereUse(ShaderDefinitions definitions, Context context, AmpereOp x)
        {
            switch (x.Op)
            {
                case AmpereTable.Ipa:
                    SetUserAttributeUses(definitions, context, x[64..72] << 2, 1, false, false, false);
                    break;
                case AmpereTable.IpaIndexed:
                    SetUserAttributeUses(definitions, context, 0, 1, false, true, false);
                    break;
                case AmpereTable.Ald:
                    SetUserAttributeUses(definitions, context, x[40..50], x[74..76] + 1, x[79], x[77], x[76]);
                    break;
                case AmpereTable.Ast:
                    SetUserAttributeUses(definitions, context, x[40..50], x[74..76] + 1, true, x[77], x[76]);
                    break;
                default:
                    context.SetUsedFeature(AmpereTable.Features(x.Op));
                    break;
            }
        }
    }
}
