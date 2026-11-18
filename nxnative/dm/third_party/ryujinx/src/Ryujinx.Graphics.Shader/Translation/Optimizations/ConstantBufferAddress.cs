using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System;
using System.Collections.Generic;
using System.Numerics;

using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation.Optimizations
{
    static class ConstantBufferAddress
    {
        private const int MaxDepth = 32;

        // Exact bit-vector form: (Scale * Root + Offset) modulo 2^Width.
        // Unknown operations are opaque SSA roots, never assumptions about their values.
        private readonly record struct Affine(Operand Root, uint Scale, uint Offset, int Width, int Cost)
        {
            public bool IsConstant => Root == null;
            public int EmitCost => IsConstant ? 0 : (Scale != 1 ? 1 : 0) + (Offset != 0 ? 1 : 0) + (Width != 32 ? 1 : 0);
        }

        public static void RunPass(BasicBlock block)
        {
            var analysis = new Dictionary<Operand, Affine>();
            var emitted = new Dictionary<(Operand, uint, uint, int), Operand>();

            for (var node = block.Operations.First; node != null; node = node.Next)
            {
                if (node.Value is not Operation operation || operation.Inst != Instruction.Load ||
                    operation.StorageKind != StorageKind.ConstantBuffer || operation.SourcesCount != 4)
                {
                    continue;
                }

                // nxnative: a byte/word address can hide a provably constant vec4 lane.
                // Dynamic lanes caused scalar selection/load ladders on Turnip even for
                // aligned matrix rows. Preserve wrap bits and all FP/memory operations.
                Operand lane = operation.GetSource(3);
                Affine component = Analyze(lane, analysis, 0);

                if (lane.Type == OperandType.Constant || !component.IsConstant || component.Offset > 3)
                {
                    continue;
                }

                Operand index = operation.GetSource(2);
                Affine vector = Analyze(index, analysis, 0);

                if (vector.EmitCost < vector.Cost)
                {
                    operation.SetSource(2, Emit(vector, block, node, emitted));
                }

                operation.SetSource(3, Const(unchecked((int)component.Offset)));
            }
        }

        private static uint Mask(int width) => width == 32 ? uint.MaxValue : (1u << width) - 1;

        private static Affine Form(Operand root, uint scale, uint offset, int width, int cost)
        {
            uint mask = Mask(width);
            scale &= mask;
            offset &= mask;
            return new Affine(scale == 0 ? null : root, scale, offset, width, cost);
        }

        private static Affine Analyze(Operand value, Dictionary<Operand, Affine> memo, int depth)
        {
            if (value.Type == OperandType.Constant)
            {
                return new Affine(null, 0, unchecked((uint)value.Value), 32, 0);
            }

            if (depth >= MaxDepth)
            {
                return new Affine(value, 1, 0, 32, 0);
            }

            if (memo.TryGetValue(value, out Affine cached))
            {
                return cached;
            }

            Affine result = new(value, 1, 0, 32, 0);

            if (value.AsgOp is Operation op && op.Dest == value)
            {
                if (op.Inst == Instruction.Copy && op.SourcesCount == 1)
                {
                    result = Analyze(op.GetSource(0), memo, depth + 1);
                }
                else if (op.SourcesCount == 2)
                {
                    // Exact integer opcodes only: FP32/FP64 flags deliberately do not match.
                    Affine x = Analyze(op.GetSource(0), memo, depth + 1);
                    Affine y = Analyze(op.GetSource(1), memo, depth + 1);
                    int cost = x.Cost + y.Cost + 1;

                    switch (op.Inst)
                    {
                        case Instruction.Add:
                        case Instruction.Subtract:
                            if (x.IsConstant && y.IsConstant)
                            {
                                uint constant = unchecked(op.Inst == Instruction.Subtract ? x.Offset - y.Offset : x.Offset + y.Offset);
                                result = new Affine(null, 0, constant, 32, cost);
                            }
                            else if (x.Width == 32 && y.Width == 32 && (x.IsConstant || y.IsConstant || x.Root == y.Root))
                            {
                                bool subtract = op.Inst == Instruction.Subtract;
                                uint scale = unchecked(subtract ? x.Scale - y.Scale : x.Scale + y.Scale);
                                uint offset = unchecked(subtract ? x.Offset - y.Offset : x.Offset + y.Offset);
                                result = Form(x.Root ?? y.Root, scale, offset, 32, cost);
                            }
                            else if (y.IsConstant && TryAddConstant(x, op.Inst == Instruction.Subtract ? -(long)(int)y.Offset : (int)y.Offset, cost, out Affine sum))
                            {
                                result = sum;
                            }
                            else if (x.IsConstant && op.Inst == Instruction.Add && TryAddConstant(y, (int)x.Offset, cost, out sum))
                            {
                                result = sum;
                            }
                            break;

                        case Instruction.Multiply:
                            if (x.IsConstant && y.Width == 32)
                            {
                                result = Form(y.Root, unchecked(x.Offset * y.Scale), unchecked(x.Offset * y.Offset), 32, cost);
                            }
                            else if (y.IsConstant && x.Width == 32)
                            {
                                result = Form(x.Root, unchecked(y.Offset * x.Scale), unchecked(y.Offset * x.Offset), 32, cost);
                            }
                            break;

                        case Instruction.ShiftLeft:
                            if (y.IsConstant && y.Offset < 32)
                            {
                                int shift = (int)y.Offset;
                                result = Form(x.Root, x.Scale << shift, x.Offset << shift, Math.Min(32, x.Width + shift), cost);
                            }
                            break;

                        case Instruction.ShiftRightU32:
                        case Instruction.ShiftRightS32:
                            if (y.IsConstant && y.Offset < 32 && (op.Inst == Instruction.ShiftRightU32 || x.Width < 32))
                            {
                                int shift = (int)y.Offset;

                                if (shift >= x.Width)
                                {
                                    result = new Affine(null, 0, 0, 32, cost);
                                }
                                else if ((x.Scale & Mask(shift)) == 0)
                                {
                                    // ((a*x+b) mod 2^w) >> s = ((a/2^s)*x + b/2^s) mod 2^(w-s),
                                    // only when a is divisible by 2^s. The reduced wrap mask is essential.
                                    result = Form(x.Root, x.Scale >> shift, x.Offset >> shift, x.Width - shift, cost);
                                }
                            }
                            break;

                        case Instruction.BitwiseAnd:
                            if (x.IsConstant)
                            {
                                (x, y) = (y, x);
                            }

                            if (y.IsConstant && (y.Offset & unchecked(y.Offset + 1)) == 0)
                            {
                                int width = y.Offset == uint.MaxValue ? 32 : BitOperations.PopCount(y.Offset);
                                result = Form(x.Root, x.Scale, x.Offset, Math.Min(x.Width, width), cost);
                            }
                            break;
                    }
                }
            }

            memo[value] = result;
            return result;
        }

        private static bool TryAddConstant(Affine value, long constant, int cost, out Affine result)
        {
            // Word offsets often add the lane AFTER the byte-to-word shift. An aligned
            // value has unused low bits, so adding a small lane cannot carry out of
            // its reduced wrap width. Prove that bound rather than dropping the mask.
            int alignment = Math.Min(value.Width, BitOperations.TrailingZeroCount(value.Scale));
            uint residue = value.Offset & Mask(alignment);
            long maximum = Mask(value.Width) - Mask(alignment) + residue;

            if (residue + constant >= 0 && maximum + constant <= Mask(value.Width))
            {
                result = Form(value.Root, value.Scale, unchecked(value.Offset + (uint)constant), value.Width, cost);
                return true;
            }

            result = default;
            return false;
        }

        private static Operand Emit(Affine value, BasicBlock block, LinkedListNode<INode> before,
            Dictionary<(Operand, uint, uint, int), Operand> memo)
        {
            if (value.IsConstant)
            {
                return Const(unchecked((int)value.Offset));
            }

            var key = (value.Root, value.Scale, value.Offset, value.Width);

            if (memo.TryGetValue(key, out Operand existing))
            {
                return existing;
            }

            Operand result = value.Root;

            void Append(Instruction instruction, uint constant)
            {
                Operand dest = Local();
                block.Operations.AddBefore(before, new Operation(instruction, dest, result, Const(unchecked((int)constant))));
                result = dest;
            }

            if (value.Scale != 1)
            {
                Append(Instruction.Multiply, value.Scale);
            }

            if (value.Offset != 0)
            {
                Append(Instruction.Add, value.Offset);
            }

            if (value.Width != 32)
            {
                Append(Instruction.BitwiseAnd, Mask(value.Width));
            }

            memo[key] = result;
            return result;
        }
    }
}
