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

                        // nxnative: a call's arguments stay the locals StructuredProgram copied them to (SPIR-V passes them by pointer). Propagated, the call
                        // kept a local whose declaration was removed (KeyNotFoundException in CodeGen/Spirv GetLocalPointer: 1,894 shaders of Viractal)
                        // nxnative: a source with side effects is never propagated either: its assignment stays (below), so a copy at the use ran it twice.
                        // Pokémon Legends: Z-A's culling compute shaders (66f848f2d65e7b75, e7dd2a9e45c705b9, ec5ebd6d9902a9a1) did each warp's atomic add on a
                        // record's done counter twice; the lane whose rank equals the total then never came, the counters were never reset and grew every frame
                        // (GPU 9 → 70 ms a frame within minutes on both the Adreno driver and Turnip)
                        if (propVar.Defs.Count == 1 && isWorthPropagating && !UsedByCall(propVar) && !HasSideEffects(assignment.Source))
                        {
                            PropagateExpression(propVar, assignment.Source);
                        }

                        // nxnative: not when the source has side effects. An atomic whose result is never read went with its assignment: Tears of the Kingdom's
                        // visibility counter (vertex shader 1eee2726aae7493e, an atomic add of a ballot's count) lost the add and everything that fed it
                        if (propVar.Type == OperandType.LocalVariable && propVar.Uses.Count == 0 && !HasSideEffects(assignment.Source))
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
