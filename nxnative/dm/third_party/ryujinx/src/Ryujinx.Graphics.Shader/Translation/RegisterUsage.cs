using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Numerics;
using System.Runtime.Intrinsics;

namespace Ryujinx.Graphics.Shader.Translation
{
    static class RegisterUsage
    {
        // 64 registers a mask word: GPRs take words 0-3, then a word each for predicates, flags, uniform registers and uniform predicates
        private const int TotalMasks = 8;
        private static readonly RegisterType[] _wordType = [RegisterType.Gpr, RegisterType.Gpr, RegisterType.Gpr, RegisterType.Gpr,
                                                            RegisterType.Predicate, RegisterType.Flag, RegisterType.Uniform, RegisterType.UniformPredicate];

        private static int FirstWord(RegisterType type) => type switch
        {
            RegisterType.Gpr => 0,
            RegisterType.Predicate => 4,
            RegisterType.Flag => 5,
            RegisterType.Uniform => 6,
            RegisterType.UniformPredicate => 7,
            _ => -1,   // shadow registers live within a block
        };

        private readonly struct RegisterMask : IEquatable<RegisterMask>
        {
            private readonly Vector512<long> _masks;

            private RegisterMask(Vector512<long> masks) => _masks = masks;

            public RegisterMask(int word, long bits) => _masks = Vector512<long>.Zero.WithElement(word, bits);

            public long GetMask(int index) => _masks.GetElement(index);

            public static RegisterMask All() => new(Vector512<long>.AllBitsSet);

            public static RegisterMask operator &(RegisterMask x, RegisterMask y) => new(x._masks & y._masks);

            public static RegisterMask operator |(RegisterMask x, RegisterMask y) => new(x._masks | y._masks);

            public static RegisterMask operator ~(RegisterMask x) => new(~x._masks);

            public static bool operator ==(RegisterMask x, RegisterMask y) => x.Equals(y);

            public static bool operator !=(RegisterMask x, RegisterMask y) => !x.Equals(y);

            public override bool Equals(object obj) => obj is RegisterMask regMask && Equals(regMask);

            public bool Equals(RegisterMask other) => _masks == other._masks;

            public override int GetHashCode() => _masks.GetHashCode();
        }

        public readonly struct FunctionRegisterUsage
        {
            public Register[] InArguments { get; }
            public Register[] OutArguments { get; }

            public FunctionRegisterUsage(Register[] inArguments, Register[] outArguments)
            {
                InArguments = inArguments;
                OutArguments = outArguments;
            }
        }

        public static FunctionRegisterUsage RunPass(ControlFlowGraph cfg)
        {
            List<Register> inArguments = [];
            List<Register> outArguments = [];

            // Compute local register inputs and outputs used inside blocks.
            RegisterMask[] localInputs = new RegisterMask[cfg.Blocks.Length];
            RegisterMask[] localOutputs = new RegisterMask[cfg.Blocks.Length];

            foreach (BasicBlock block in cfg.Blocks)
            {
                for (LinkedListNode<INode> node = block.Operations.First; node != null; node = node.Next)
                {
                    Operation operation = node.Value as Operation;

                    for (int srcIndex = 0; srcIndex < operation.SourcesCount; srcIndex++)
                    {
                        Operand source = operation.GetSource(srcIndex);

                        if (source.Type != OperandType.Register)
                        {
                            continue;
                        }

                        Register register = source.GetRegister();

                        localInputs[block.Index] |= GetMask(register) & ~localOutputs[block.Index];
                    }

                    for (int dstIndex = 0; dstIndex < operation.DestsCount; dstIndex++)
                    {
                        Operand dest = operation.GetDest(dstIndex);

                        if (dest != null && dest.Type == OperandType.Register)
                        {
                            localOutputs[block.Index] |= GetMask(dest.GetRegister());
                        }
                    }
                }
            }

            // Compute global register inputs and outputs used across blocks.
            RegisterMask[] globalCmnOutputs = new RegisterMask[cfg.Blocks.Length];

            RegisterMask[] globalInputs = new RegisterMask[cfg.Blocks.Length];
            RegisterMask[] globalOutputs = new RegisterMask[cfg.Blocks.Length];

            RegisterMask allOutputs = new();
            RegisterMask allCmnOutputs = RegisterMask.All();

            bool modified;

            bool firstPass = true;

            do
            {
                modified = false;

                // Compute register outputs.
                for (int index = cfg.PostOrderBlocks.Length - 1; index >= 0; index--)
                {
                    BasicBlock block = cfg.PostOrderBlocks[index];

                    if (block.Predecessors.Count != 0)
                    {
                        BasicBlock predecessor = block.Predecessors[0];

                        RegisterMask cmnOutputs = localOutputs[predecessor.Index] | globalCmnOutputs[predecessor.Index];

                        RegisterMask outputs = globalOutputs[predecessor.Index];

                        for (int pIndex = 1; pIndex < block.Predecessors.Count; pIndex++)
                        {
                            predecessor = block.Predecessors[pIndex];

                            cmnOutputs &= localOutputs[predecessor.Index] | globalCmnOutputs[predecessor.Index];

                            outputs |= globalOutputs[predecessor.Index];
                        }

                        globalInputs[block.Index] |= outputs & ~cmnOutputs;

                        if (!firstPass)
                        {
                            cmnOutputs &= globalCmnOutputs[block.Index];
                        }

                        if (EndsWithReturn(block))
                        {
                            allCmnOutputs &= cmnOutputs | localOutputs[block.Index];
                        }

                        if (Exchange(globalCmnOutputs, block.Index, cmnOutputs))
                        {
                            modified = true;
                        }

                        outputs |= localOutputs[block.Index];

                        if (Exchange(globalOutputs, block.Index, globalOutputs[block.Index] | outputs))
                        {
                            allOutputs |= outputs;
                            modified = true;
                        }
                    }
                    else if (Exchange(globalOutputs, block.Index, localOutputs[block.Index]))
                    {
                        allOutputs |= localOutputs[block.Index];
                        modified = true;
                    }
                }

                // Compute register inputs.
                for (int index = 0; index < cfg.PostOrderBlocks.Length; index++)
                {
                    BasicBlock block = cfg.PostOrderBlocks[index];

                    RegisterMask inputs = localInputs[block.Index];

                    if (block.Next != null)
                    {
                        inputs |= globalInputs[block.Next.Index];
                    }

                    if (block.Branch != null)
                    {
                        inputs |= globalInputs[block.Branch.Index];
                    }

                    inputs &= ~globalCmnOutputs[block.Index];

                    if (Exchange(globalInputs, block.Index, globalInputs[block.Index] | inputs))
                    {
                        modified = true;
                    }
                }

                firstPass = false;
            }
            while (modified);

            // Insert load and store context instructions where needed.
            foreach (BasicBlock block in cfg.Blocks)
            {
                // The only block without any predecessor should be the entry block.
                // It always needs a context load as it is the first block to run.
                if (block.Predecessors.Count == 0)
                {
                    RegisterMask inputs = globalInputs[block.Index] | (allOutputs & ~allCmnOutputs);

                    LoadLocals(block, inputs, inArguments);
                }

                if (EndsWithReturn(block))
                {
                    StoreLocals(block, allOutputs, inArguments.Count, outArguments);
                }
            }

            return new FunctionRegisterUsage(inArguments.ToArray(), outArguments.ToArray());
        }

        public static void FixupCalls(BasicBlock[] blocks, FunctionRegisterUsage[] frus)
        {
            foreach (BasicBlock block in blocks)
            {
                for (LinkedListNode<INode> node = block.Operations.First; node != null; node = node.Next)
                {
                    Operation operation = node.Value as Operation;

                    if (operation.Inst == Instruction.Call)
                    {
                        Operand funcId = operation.GetSource(0);

                        Debug.Assert(funcId.Type == OperandType.Constant);

                        FunctionRegisterUsage fru = frus[funcId.Value];

                        Operand[] inRegs = new Operand[fru.InArguments.Length];

                        for (int i = 0; i < fru.InArguments.Length; i++)
                        {
                            inRegs[i] = OperandHelper.Register(fru.InArguments[i]);
                        }

                        operation.AppendSources(inRegs);

                        Operand[] outRegs = new Operand[1 + fru.OutArguments.Length];

                        for (int i = 0; i < fru.OutArguments.Length; i++)
                        {
                            outRegs[1 + i] = OperandHelper.Register(fru.OutArguments[i]);
                        }

                        operation.AppendDests(outRegs);
                    }
                }
            }
        }

        private static bool StartsWith(BasicBlock block, Instruction inst)
        {
            if (block.Operations.Count == 0)
            {
                return false;
            }

            return block.Operations.First.Value is Operation operation && operation.Inst == inst;
        }

        private static bool EndsWith(BasicBlock block, Instruction inst)
        {
            if (block.Operations.Count == 0)
            {
                return false;
            }

            return block.Operations.Last.Value is Operation operation && operation.Inst == inst;
        }

        private static RegisterMask GetMask(Register register)
        {
            int word = FirstWord(register.Type);

            return word < 0 ? new() : new(word + (register.Index >> 6), 1L << (register.Index & 0x3f));
        }

        private static bool Exchange(RegisterMask[] masks, int blkIndex, RegisterMask value)
        {
            RegisterMask oldValue = masks[blkIndex];

            masks[blkIndex] = value;

            return oldValue != value;
        }

        private static void LoadLocals(BasicBlock block, RegisterMask masks, List<Register> inArguments)
        {
            bool fillArgsList = inArguments.Count == 0;
            LinkedListNode<INode> node = null;
            int argIndex = 0;

            for (int i = 0; i < TotalMasks; i++)
            {
                (RegisterType regType, int baseRegIndex) = GetRegTypeAndBaseIndex(i);
                long mask = masks.GetMask(i);

                while (mask != 0)
                {
                    int bit = BitOperations.TrailingZeroCount(mask);

                    mask &= ~(1L << bit);

                    Register register = new(baseRegIndex + bit, regType);

                    if (fillArgsList)
                    {
                        inArguments.Add(register);
                    }

                    Operation copyOp = new(Instruction.Copy, OperandHelper.Register(register), OperandHelper.Argument(argIndex++));

                    if (node == null)
                    {
                        node = block.Operations.AddFirst(copyOp);
                    }
                    else
                    {
                        node = block.Operations.AddAfter(node, copyOp);
                    }
                }
            }

            Debug.Assert(argIndex <= inArguments.Count);
        }

        private static void StoreLocals(BasicBlock block, RegisterMask masks, int inArgumentsCount, List<Register> outArguments)
        {
            LinkedListNode<INode> node = null;
            int argIndex = inArgumentsCount;
            bool fillArgsList = outArguments.Count == 0;

            for (int i = 0; i < TotalMasks; i++)
            {
                (RegisterType regType, int baseRegIndex) = GetRegTypeAndBaseIndex(i);
                long mask = masks.GetMask(i);

                while (mask != 0)
                {
                    int bit = BitOperations.TrailingZeroCount(mask);

                    mask &= ~(1L << bit);

                    Register register = new(baseRegIndex + bit, regType);

                    if (fillArgsList)
                    {
                        outArguments.Add(register);
                    }

                    Operation copyOp = new(Instruction.Copy, OperandHelper.Argument(argIndex++), OperandHelper.Register(register));

                    if (node == null)
                    {
                        node = block.Operations.AddBefore(block.Operations.Last, copyOp);
                    }
                    else
                    {
                        node = block.Operations.AddAfter(node, copyOp);
                    }
                }
            }

            Debug.Assert(argIndex <= inArgumentsCount + outArguments.Count);
        }

        private static (RegisterType RegType, int BaseRegIndex) GetRegTypeAndBaseIndex(int i)
        {
            RegisterType type = _wordType[i];

            return (type, (i - FirstWord(type)) * 64);
        }

        private static bool EndsWithReturn(BasicBlock block)
        {
            if (block.GetLastOp() is not Operation operation)
            {
                return false;
            }

            return operation.Inst == Instruction.Return;
        }
    }
}
