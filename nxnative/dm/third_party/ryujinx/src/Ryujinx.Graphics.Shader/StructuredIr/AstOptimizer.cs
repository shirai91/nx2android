using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using System.Collections.Generic;
using System.Linq;

using static Ryujinx.Graphics.Shader.StructuredIr.AstHelper;

namespace Ryujinx.Graphics.Shader.StructuredIr
{
    static class AstOptimizer
    {
        public static void Optimize(StructuredProgramContext context, TargetLanguage targetLanguage)
        {
            AstBlock mainBlock = context.CurrentFunction.MainBlock;

            // When debug mode is enabled, we disable expression propagation
            // (this makes comparison with the disassembly easier).
            if (!context.DebugMode)
            {
                AstBlockVisitor visitor = new(mainBlock);

                foreach (IAstNode node in visitor.Visit())
                {
                    if (node is AstAssignment assignment && assignment.Destination is AstOperand propVar)
                    {
                        bool isWorthPropagating = propVar.Uses.Count == 1 || IsWorthPropagating(assignment.Source);

                        // nxnative: an operation with side effects stays where it is, in its assignment. An atomic whose result is never read went with its
                        // assignment (Tears of the Kingdom's visibility counter, vertex shader 1eee2726aae7493e, lost its add); one propagated into its use as
                        // well ran twice (Donkey Kong Bananza's terrain geometry shader 5d537f7630ec3671 counted every triangle twice)
                        bool sideEffects = HasSideEffects(assignment.Source);

                        // nxnative: a call's arguments stay the locals StructuredProgram copied them to (SPIR-V passes them by pointer). Propagated, the call
                        // kept a local whose declaration was removed (KeyNotFoundException in CodeGen/Spirv GetLocalPointer: 1,894 shaders of Viractal)
                        if (propVar.Defs.Count == 1 && isWorthPropagating && !sideEffects && !IsWarpWide(assignment.Source) && !UsedByCall(propVar))
                        {
                            PropagateExpression(propVar, assignment.Source);
                        }

                        if (propVar.Type == OperandType.LocalVariable && propVar.Uses.Count == 0 && !sideEffects)
                        {
                            visitor.Block.Remove(assignment);

                            context.CurrentFunction.Locals.Remove(propVar);
                        }
                    }
                }
            }

            RemoveEmptyBlocks(mainBlock, targetLanguage);
        }

        private static bool HasSideEffects(IAstNode source)
        {
            return source is AstOperation operation && (operation.Inst & Instruction.Mask) switch
            {
                Instruction.AtomicAdd or Instruction.AtomicAnd or Instruction.AtomicCompareAndSwap or Instruction.AtomicMaxS32 or Instruction.AtomicMaxU32 or
                Instruction.AtomicMinS32 or Instruction.AtomicMinU32 or Instruction.AtomicOr or Instruction.AtomicSwap or Instruction.AtomicXor or
                Instruction.Call or Instruction.ImageAtomic => true,
                _ => false,
            };
        }

        // nxnative: an operation over the warp's lanes runs where the code put it. Propagated into its one use, Ampere's REDUX moved into the branch only the warp's leader
        // takes, and reduced over that one lane (Donkey Kong Bananza's compute shaders 72b4adba0a491c21 and c5b4ee0ce9a02d7d)
        private static bool IsWarpWide(IAstNode source)
        {
            return source is AstOperation operation && (operation.Inst & Instruction.Mask) is Instruction.Ballot or Instruction.Shuffle or Instruction.ShuffleDown or
                Instruction.ShuffleUp or Instruction.ShuffleXor or Instruction.SubgroupReduce or Instruction.VoteAll or Instruction.VoteAllEqual or Instruction.VoteAny or
                Instruction.Ddx or Instruction.Ddy;
        }

        private static bool UsedByCall(AstOperand propVar)
        {
            foreach (IAstNode use in propVar.Uses)
            {
                if (use is AstOperation operation && operation.Inst == Instruction.Call)
                {
                    return true;
                }
            }

            return false;
        }

        private static bool IsWorthPropagating(IAstNode source)
        {
            if (source is not AstOperation srcOp)
            {
                return false;
            }

            if (!InstructionInfo.IsUnary(srcOp.Inst))
            {
                return false;
            }

            return srcOp.GetSource(0) is AstOperand || srcOp.Inst == Instruction.Copy;
        }

        private static void PropagateExpression(AstOperand propVar, IAstNode source)
        {
            IAstNode[] uses = propVar.Uses.ToArray();

            foreach (IAstNode useNode in uses)
            {
                if (useNode is AstBlock useBlock)
                {
                    useBlock.Condition = source;
                }
                else if (useNode is AstOperation useOperation)
                {
                    for (int srcIndex = 0; srcIndex < useOperation.SourcesCount; srcIndex++)
                    {
                        if (useOperation.GetSource(srcIndex) == propVar)
                        {
                            useOperation.SetSource(srcIndex, source);
                        }
                    }
                }
                else if (useNode is AstAssignment useAssignment)
                {
                    useAssignment.Source = source;
                }
            }
        }

        private static void RemoveEmptyBlocks(AstBlock mainBlock, TargetLanguage targetLanguage)
        {
            Queue<AstBlock> pending = new();

            pending.Enqueue(mainBlock);

            while (pending.TryDequeue(out AstBlock block))
            {
                foreach (IAstNode node in block)
                {
                    if (node is AstBlock childBlock)
                    {
                        pending.Enqueue(childBlock);
                    }
                }

                AstBlock parent = block.Parent;

                if (parent == null)
                {
                    continue;
                }

                AstBlock nextBlock = Next(block) as AstBlock;

                bool hasElse = nextBlock != null && nextBlock.Type == AstBlockType.Else;

                bool isIf = block.Type == AstBlockType.If;

                if (block.Count == 0)
                {
                    if (isIf)
                    {
                        if (hasElse)
                        {
                            nextBlock.TurnIntoIf(InverseCond(block.Condition));
                        }

                        parent.Remove(block);
                    }
                    else if (block.Type == AstBlockType.Else)
                    {
                        parent.Remove(block);
                    }
                }
                // nxnative: not for SPIR-V. Its generator branches from an If only into an Else that follows it, never into an ElseIf, so the else-if's whole branch
                // became unreachable code. Xenoblade Chronicles 3's volumetric cloud shaders (6a3bebeab80d95cb, fabb0ef84521a927) lost a loop's exit that way and
                // spun until the GPU was reset (device lost on Adreno, a few seconds into the title)
                else if (targetLanguage != TargetLanguage.Spirv && isIf && parent.Type == AstBlockType.Else && parent.Count == (hasElse ? 2 : 1))
                {
                    AstBlock parentOfParent = parent.Parent;

                    parent.Remove(block);

                    parentOfParent.AddAfter(parent, block);

                    if (hasElse)
                    {
                        parent.Remove(nextBlock);

                        parentOfParent.AddAfter(block, nextBlock);
                    }

                    parentOfParent.Remove(parent);

                    block.TurnIntoElseIf();
                }
            }
        }
    }
}
