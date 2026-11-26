using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System;

namespace Ryujinx.Graphics.Shader.Translation.Optimizations
{
    static class BranchElimination
    {
        public static bool RunPass(BasicBlock block)
        {
            // nxnative: a branch to a block that carries a PhiNode is not redundant even when the
            // fallthrough block holds only an unconditional jump to the same target. Optimizer.PropagateCopy
            // folds a single-copy fallthrough block (such as the predicated-SYNC tail in NieR:Automata's
            // Forward+ fragment shaders) into the target's PhiNode, leaving only its Branch; clearing
            // block.Branch made block fall straight into the fallthrough block's phi copy and
            // unconditionally overwrite its own incoming value. Ssa.AddPhi keeps all PhiNodes at the
            // front of a block's Operations, so checking First is sufficient.
            if (block.HasBranch && block.Branch.Operations.First?.Value is not PhiNode && IsRedundantBranch((Operation)block.GetLastOp(), Next(block)))
            {
                block.Branch = null;

                return true;
            }

            return false;
        }

        private static bool IsRedundantBranch(Operation current, BasicBlock nextBlock)
        {
            // Here we check that:
            // - The current block ends with a branch.
            // - The next block only contains a branch.
            // - The branch on the next block is unconditional.
            // - Both branches are jumping to the same location.
            // In this case, the branch on the current block can be removed,
            // as the next block is going to jump to the same place anyway.

            if (nextBlock?.Operations.First?.Value is not Operation next)
            {
                return false;
            }

            if (next.Inst != Instruction.Branch)
            {
                return false;
            }

            return current.Dest == next.Dest;
        }

        private static BasicBlock Next(BasicBlock block)
        {
            block = block.Next;

            while (block != null && block.Operations.Count == 0)
            {
                if (block.HasBranch)
                {
                    throw new InvalidOperationException("Found a bogus empty block that \"ends with a branch\".");
                }

                block = block.Next;
            }

            return block;
        }
    }
}
