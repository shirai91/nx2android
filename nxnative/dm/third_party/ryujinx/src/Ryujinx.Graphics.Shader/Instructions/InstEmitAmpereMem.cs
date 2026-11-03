using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using System.Collections.Generic;
using System.Linq;
using System.Numerics;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Instructions
{
    // Ampere attribute, memory, texture and surface instructions. Field positions: Mesa NAK sm70_encode.rs (OpIpa, OpALd, OpLd, OpSt, OpAtom, OpTex ...).
    static partial class InstEmit
    {
        public static void RegisterAmpereMemory(AmpereExact x)
        {
            const AmpereProps N = AmpereProps.NoRd, P = AmpereProps.Pure, None = AmpereProps.None;
            const FeatureFlags Store = FeatureFlags.Store, Shared = FeatureFlags.SharedMemory, Local = FeatureFlags.LocalMemory;

            x(AmpereTable.Ipa, InstName.Ipa, AIpa, P);
            x(AmpereTable.IpaIndexed, InstName.Ipa, AIpa, P);
            x(AmpereTable.Ald, InstName.Ald, AAld, P);
            x(AmpereTable.Ast, InstName.Ast, AAst, N);
            x(0x324, InstName.Out, AOut, None);
            x(0x923, InstName.Isberd, AIsberd, P);
            x(0x34e, InstName.Mov, ALepc, P);

            x(0x381, InstName.Ldg, ALdg, None);
            x(0x981, InstName.Ldg, ALdg, None);
            x(0x386, InstName.Stg, AStg, N, Store);
            x(0x986, InstName.Stg, AStg, N, Store);
            x(0x98e, InstName.Red, AAtomg, N, Store);
            x(0x3a8, InstName.Atom, AAtomg, None, Store);
            x(0x9a8, InstName.Atom, AAtomg, None, Store);
            x(0x983, InstName.Ldl, ALdLocalShared, None, Local);
            x(0x984, InstName.Lds, ALdLocalShared, None, Shared);
            x(0x387, InstName.Stl, AStLocalShared, N, Local);
            x(0x987, InstName.Stl, AStLocalShared, N, Local);
            x(0x388, InstName.Sts, AStLocalShared, N, Shared);
            x(0x988, InstName.Sts, AStLocalShared, N, Shared);
            x(0x38c, InstName.Atoms, AAtoms, None, Shared);
            x(0x98c, InstName.Atoms, AAtoms, None, Shared);
            x(0x38d, InstName.Atoms, AAtoms, None, Shared);
            x(0x992, InstName.Membar, _ => { }, N);
            x(0x9ab, InstName.Membar, _ => { }, N);
            x(0xb1d, InstName.Bar, ABar, N);

            foreach (int op in new[] { 0xb60, 0xb66, 0xb63, 0xb6c })
            {
                x(op, InstName.Tex, ATex, P);
            }

            x(0x361, InstName.Tex, ATex, P, FeatureFlags.Bindless);

            x(0xb69, InstName.Tmml, ATmml, P);
            x(0xb6f, InstName.Txq, ATxq, P);
            x(0xb97, InstName.Suld, ASurface, None);
            x(0xb99, InstName.Suld, ASurface, None);
            x(0xb9b, InstName.Sust, ASurface, N, Store);
            x(0xb9d, InstName.Sust, ASurface, N, Store);
            x(0x593, InstName.Suatom, ASuatom, None, Store);
        }

        // ---- attributes and stage buffers ----

        // IPA Rd, a[addr] (addr 64..72 in words; 0x526: a[URa + addr], URa at 24..30). Location 76..78 (offset not met), frequency 78..80. As Maxwell's IPA.PASS, a
        // perspective attribute is read divided by w and the shader multiplies by w itself
        public static void AIpa(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int attr = x[64..72] << 2;

            if (x[76..78] == 2)
            {
                throw AFail(context, "IPA at an offset");
            }

            Operand index = x.Op == AmpereTable.IpaIndexed ? context.IAdd(Register(x[24..30], RegisterType.Uniform), Const(attr)) : null;
            ASet(context, x, EmitIpa(context, index, attr, out _));
        }

        // ALD Rd, a[Ra + addr], vtx: vertex 32..40, addr 40..50, count - 1 74..76, patch 76, physical 77, output 79
        public static void AAld(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            EmitAld(context, x.Rd, x.Ra, x.Rb, x[40..50], x[74..76], x[76], x[79], x[77]);
        }

        // AST a[Ra + addr], data (32..40): as ALD's fields
        public static void AAst(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            EmitAst(context, x.Ra, x.Rb, x[40..50], x[74..76], x[76], x[77]);
        }

        // OUT Rd, Ra, stream: emit (78..80 = 1), cut (2) or both; Rd the next output handle, which is Ra's
        public static void AOut(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int type = x[78..80];

            if (type != 0)
            {
                EmitOut(context, (type & 1) != 0, (type & 2) != 0);
            }

            ASet(context, x, AGpr(x.Ra));
        }

        // ISBERD Rd, [Ra]: the stage buffer entry of a vertex, which only ALD reads: Ra passes through, as on Maxwell
        public static void AIsberd(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            ASet(context, x, AGpr(x.Ra));
        }

        // LEPC Rd: the instruction's own address, read only to set up an EXIT continuation (which the host has no use for)
        public static void ALepc(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            ASet(context, x, Const((int)context.CurrOp.Address));
            ASet(context, x, Const(0), 1);
        }

        // ---- global memory ----

        // The address of a global access: Ra (a pair with pairBit), plus a uniform register pair at urAt (bit 91; a pair with 72), plus the offset 40..64, with carries
        private static (Operand Lo, Operand Hi) AGlobalAddress(EmitterContext context, AmpereOp x, int pairBit, int urAt, int extra)
        {
            Operand lo = AGpr(x.Ra), hi = x[pairBit] ? AGpr(x.Ra, 1) : Const(0);
            int ur = x[urAt..(urAt + 6)];

            if (x[91] && ur != RegisterConsts.UniformZeroIndex)
            {
                (lo, Operand carry) = AAddCarry(context, Register(ur, RegisterType.Uniform), lo, null);
                hi = context.IAdd(context.IAdd(hi, x[72] ? Register(ur + 1, RegisterType.Uniform) : Const(0)), carry);
            }

            int offset = (int)x.Signed(40..64) + extra;

            if (offset != 0)
            {
                Operand sum = context.IAdd(lo, Const(offset));
                Operand borrow = offset > 0 ? context.ICompareLessUnsigned(sum, lo) : context.ICompareGreaterUnsigned(sum, lo);
                hi = context.IAdd(hi, context.ConditionalSelect(borrow, Const(offset > 0 ? 1 : -1), Const(0)));
                lo = sum;
            }

            return (lo, hi);
        }

        // LDG Rd, [Ra + UR + offset]: size 73..76. 0x981 (pair bit 90, UR at 32) loads only where the predicate 64..67 (stored as 7 - P, 67 inverts) holds, zero
        // elsewhere; 0x381 (pair bit 72) has neither
        public static void ALdg(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            LsSize size = (LsSize)x[73..76];
            int count = GetVectorCount(size);
            bool ugpr = x.Op == 0x981;
            (Operand lo, Operand hi)[] addrs = Enumerable.Range(0, count).Select(i => AGlobalAddress(context, x, ugpr ? 90 : 72, 32, 4 * i)).ToArray();
            Operand pred = ugpr && !(x[64..67] == 0 && !x[67]) ? Register(7 - x[64..67], RegisterType.Predicate) : null;
            Operand skip = pred == null ? null : Label();

            if (skip != null)
            {
                for (int i = 0; i < count && x.Rd + i < Rz; i++)
                {
                    ASet(context, x, Const(0), i);
                }

                context.BranchIfFalse(skip, x[67] ? context.BitwiseNot(pred) : pred);
            }

            for (int i = 0; i < count && x.Rd + i < Rz; i++)
            {
                ASet(context, x, context.Load(GetStorageKind(size), addrs[i].lo, addrs[i].hi), i);
            }

            AEndSkip(context, skip);
        }

        // STG [Ra + UR + offset], Rb: 0x986 (pair bit 90, UR at 64) or 0x386 (pair bit 72)
        public static void AStg(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            LsSize size = (LsSize)x[73..76];
            bool ugpr = x.Op == 0x986;

            for (int i = 0; i < GetVectorCount(size); i++)
            {
                (Operand lo, Operand hi) = AGlobalAddress(context, x, ugpr ? 90 : 72, 64, 4 * i);
                context.Store(GetStorageKind(size), lo, hi, AGpr(x.Rb, x.Rb == Rz ? 0 : i));
            }
        }

        // RED (0x98e, operation 87..90) and ATOMG (0x9a8 / 0x3a8, operation 87..91) [Ra + UR + offset], Rb: type 73..76; UR at 64; Ra's pair bit 90 (RED), 70 (0x9a8) or 72
        public static void AAtomg(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int pairBit = x.Op switch { 0x98e => 90, 0x9a8 => 70, _ => 72 };
            (Operand lo, Operand hi) = AGlobalAddress(context, x, pairBit, 64, 0);
            AtomOp op = (AtomOp)(x.Op == 0x98e ? x[87..90] : x[87..91]);
            Operand res = EmitAtomicOp(context, StorageKind.GlobalMemory, op, (AtomSize)x[73..76], lo, hi, AGpr(x.Rb));

            if (x.Op != 0x98e)
            {
                ASet(context, x, res);
            }
        }

        // ---- local and shared memory ----

        // A local / shared address: Ra counted in elements of 1, 4, 8 or 16 bytes (stride 78..80, shared only), plus the uniform register at urAt (bit 91)
        private static Operand ALocalAddress(EmitterContext context, AmpereOp x, int urAt, bool shared)
        {
            Operand addr = AGpr(x.Ra);
            int stride = shared ? x[78..80] : 0;

            if (stride != 0 && x.Ra != Rz)
            {
                addr = context.ShiftLeft(addr, Const(stride + 1));
            }

            int ur = x[urAt..(urAt + 6)];

            return x[91] && ur != RegisterConsts.UniformZeroIndex ? context.IAdd(addr, Register(ur, RegisterType.Uniform)) : addr;
        }

        // LDL / LDS Rd, [Ra + UR + offset]: UR at 32, size 73..76, offset 40..64
        public static void ALdLocalShared(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool shared = x.Op == 0x984;
            EmitLoad(context, shared ? StorageKind.SharedMemory : StorageKind.LocalMemory, (LsSize2)x[73..76], ALocalAddress(context, x, 32, shared), x.Rd, (int)x.Signed(40..64));
        }

        // STL / STS [Ra + UR + offset], Rb: UR at 64
        public static void AStLocalShared(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool shared = x.Op is 0x388 or 0x988;
            EmitStore(context, shared ? StorageKind.SharedMemory : StorageKind.LocalMemory, (LsSize2)x[73..76], ALocalAddress(context, x, 64, shared), x.Rb, (int)x.Signed(40..64));
        }

        // ATOMS Rd, [Ra + UR + offset], Rb (operation 87..91, size 73..76). ATOMS.CAS (0x38d) Rd, [Ra + offset], Rb (compare), Rc (new value): Rd = the old value
        public static void AAtoms(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            Operand word = context.ShiftRightU32(context.IAdd(ALocalAddress(context, x, 64, true), Const((int)x.Signed(40..64))), Const(2));
            Operand id = Const(context.ResourceManager.SharedMemoryId);

            Operand res = x.Op == 0x38d
                ? context.AtomicCompareAndSwap(StorageKind.SharedMemory, id, word, AGpr(x.Rb), AGpr(x.Rc))
                : EmitAtomicOp(context, StorageKind.SharedMemory, (AtomOp)x[87..91], (AtomSize)x[73..76], id, word, AGpr(x.Rb));

            ASet(context, x, res);
        }

        // BAR.SYNC (mode 77..80: 0 sync, 4 its arrive-and-wait form)
        public static void ABar(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x[77..80] is not (0 or 4))
            {
                throw AFail(context, $"BAR mode {x[77..80]}");
            }

            context.Barrier();
        }

        // ---- textures ----

        // The handle of a texture or surface: the word offset 40..54 in the bank 54..59 (NVN's handle table c[2]; NVN2 compute's c[0] is mapped there)
        private static int AHandle(EmitterContext context, AmpereOp x)
        {
            (int bank, int offset) = AMapC0(context, x[54..59], x[40..54] * 4);

            if (bank != 2)
            {
                throw AFail(context, $"texture handle in bank {bank}");
            }

            return offset >> 2;
        }

        // The source registers of a texture instruction: n0 from Ra, then n1 from Rb. With .SCR (60) the sequence is spread over the two registers instead
        // (4 as 2 + 2, 3 as 2 + 1, 2 as 1 + 1; NAK try_scalarize_tex)
        private static (List<Operand> A, List<Operand> B) ATexSources(EmitterContext context, AmpereOp x, int n0, int n1)
        {
            int total = n0 + n1, fromA = x[60] ? (total >= 3 ? 2 : 1) : n0;

            if (x[60] && total > 4)
            {
                throw AFail(context, "texture .SCR with more than four sources");
            }

            List<Operand> seq = [];
            seq.AddRange(Enumerable.Range(0, System.Math.Min(fromA, total)).Select(k => context.Copy(AGpr(x.Ra, x.Ra == Rz ? 0 : k))));
            seq.AddRange(Enumerable.Range(0, total - seq.Count).Select(k => context.Copy(AGpr(x.Rb, x.Rb == Rz ? 0 : k))));

            return (seq.GetRange(0, n0), seq.GetRange(n0, n1));
        }

        // The register of result k (mask 72..76; .F16 (79): of pair k): Rd + k, but with a second destination Rc (64..72) from the third (the second pair) on Rc + ...
        private static int ATexReg(AmpereOp x, int k, bool f16)
        {
            int split = f16 ? 1 : 2;

            return x.Rc != Rz && k >= split ? x.Rc + k - split : x.Rd + k;
        }

        private static Operand[] ATexDests(AmpereOp x, int count, bool f16)
        {
            return Enumerable.Range(0, count).Select(i => !f16 && ATexReg(x, i, false) < Rz ? Register(ATexReg(x, i, false), RegisterType.Gpr) : Local()).ToArray();
        }

        // TEX (0xb60; 0x361 the handle in a register), TLD (0xb66), TLD4 (0xb63), TXD (0xb6c). Dimension 61..64, mask 72..76, .F16 79, residency predicate 81..84
        // (not met). Sources (NAK nak_nir_lower_tex, SM70): Ra = array index, coordinates (TXD: coordinates, then the array index with the offsets in its high half);
        // Rb = LOD / bias, offset, depth reference, sample (TXD: the derivatives, x and y interleaved)
        public static void ATex(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int op = x.Op == 0x361 ? 0xb60 : x.Op;
            bool bindless = x.Op == 0x361;
            SamplerType type = ConvertSamplerType(ATexDim(context, x[61..64]));
            bool array = type.HasFlag(SamplerType.Array);
            int coords = type.Dimensions;
            TextureFlags flags = bindless ? TextureFlags.Bindless : TextureFlags.None;
            int lod = 0, offMode = 0;
            bool depth = false, ms = false;

            if (x[81..84] != Pt)
            {
                throw AFail(context, "texture residency predicate");
            }

            switch (op)
            {
                case 0xb60:   // lod 87..90 (0 auto, 1 zero, 2 bias, 3 level), offset 76, depth reference 78
                    (lod, offMode, depth) = (x[87..90], x[76] ? 1 : 0, x[78]);
                    break;
                case 0xb66:   // lod (1 zero, 3 level), offset 76, multisample 78
                    (lod, offMode, ms) = (x[87..90], x[76] ? 1 : 0, x[78]);
                    flags |= TextureFlags.IntCoords;
                    break;
                case 0xb63:   // offset mode 76..78 (1 one offset, 2 one per texel), depth reference 78, component 87..89
                    (offMode, depth) = (x[76..78], x[78]);
                    flags |= TextureFlags.Gather;
                    break;
                case 0xb6c:
                    offMode = x[76] ? 1 : 0;
                    flags |= TextureFlags.Derivatives;
                    break;
            }

            if (lod > 3 || offMode > 2 || (op == 0xb66 && lod is not (1 or 3)))
            {
                throw AFail(context, $"texture lod {lod} / offset mode {offMode}");
            }

            int handle = bindless ? 0 : AHandle(context, x);
            List<Operand> src = [];
            Operand[] offsets = [];

            if (op == 0xb6c)
            {
                (List<Operand> a, List<Operand> b) = ATexSources(context, x, coords + (array || offMode != 0 ? 1 : 0), 2 * coords);
                src.AddRange(a.Take(coords));

                if (array)
                {
                    src.Add(context.BitwiseAnd(a[coords], Const(0xffff)));
                }

                src.AddRange(b);
                offsets = offMode != 0 ? Enumerable.Range(0, coords).Select(i => context.BitfieldExtractS32(a[coords], Const(16 + 4 * i), Const(4))).ToArray() : [];
            }
            else
            {
                bool hasLod = (op == 0xb60 && lod >= 2) || (op == 0xb66 && lod == 3);
                int n1 = (bindless ? 1 : 0) + (hasLod ? 1 : 0) + (offMode == 2 ? 2 : offMode) + (depth ? 1 : 0) + (ms ? 1 : 0);
                (List<Operand> a, List<Operand> b) = ATexSources(context, x, coords + (array ? 1 : 0), n1);
                Queue<Operand> qb = new(b);

                if (bindless)
                {
                    src.Add(qb.Dequeue());
                }

                src.AddRange(a.Skip(array ? 1 : 0));

                if (array)
                {
                    src.Add(a[0]);
                }

                Operand lodValue = hasLod ? qb.Dequeue() : null;
                Operand[] packed = Enumerable.Range(0, offMode == 2 ? 2 : offMode).Select(_ => qb.Dequeue()).ToArray();

                if (depth)
                {
                    src.Add(qb.Dequeue());
                    type |= SamplerType.Shadow;
                }

                if (!ms && (lod == 1 || lod == 3) && op != 0xb63)
                {
                    src.Add(lodValue ?? (op == 0xb66 ? Const(0) : ConstF(0)));
                    flags |= TextureFlags.LodLevel;
                }

                // TEX / TLD: 4 bits per coordinate; TLD4: 6 bits a byte, for one texel or four
                Operand Offset(int word, int bit, int bits) => context.BitfieldExtractS32(packed[word], Const(bit), Const(bits));

                offsets = packed.Length == 0 ? []
                    : op != 0xb63 ? Enumerable.Range(0, coords).Select(i => Offset(0, 4 * i, 4)).ToArray()
                    : Enumerable.Range(0, coords * (offMode == 2 ? 4 : 1)).Select(i => Offset(i >> 2, (i & 3) * 8, 6)).ToArray();

                if (lod == 2)
                {
                    src.Add(lodValue);
                    flags |= TextureFlags.LodBias;
                }

                if (ms)
                {
                    src.Add(qb.Dequeue());
                    type |= SamplerType.Multisample;
                }
            }

            if (offsets.Length != 0)
            {
                src.AddRange(offsets);
                flags |= offMode == 2 ? TextureFlags.Offsets : TextureFlags.Offset;
            }

            if (op == 0xb63 && !depth)
            {
                src.Add(Const(x[87..89]));
            }

            // A 1D fetch with no extra source may read a texture buffer, which the answer table tells
            if (op == 0xb66 && type == SamplerType.Texture1D && lod == 1 && offMode == 0 && !bindless &&
                context.TranslatorContext.GpuAccessor.QuerySamplerType(handle) == SamplerType.TextureBuffer)
            {
                type = SamplerType.TextureBuffer;
                src.RemoveAt(src.Count - 1);
                flags &= ~TextureFlags.LodLevel;
            }

            int mask = x[72..76];
            bool f16 = x[79] && op != 0xb6c;
            Operand[] dests = ATexDests(x, BitOperations.PopCount((uint)mask), f16);

            EmitTextureSample(context, type, flags, handle, mask, dests, src.ToArray());
            ATexPackF16(context, x, dests, f16);
        }

        // .F16 results: packed two a register
        private static void ATexPackF16(EmitterContext context, AmpereOp x, Operand[] dests, bool f16)
        {
            for (int i = 0; f16 && i < dests.Length; i += 2)
            {
                ASet(context, AGpr(ATexReg(x, i / 2, true)), context.PackHalf2x16(dests[i], i + 1 < dests.Length ? dests[i + 1] : ConstF(0)));
            }
        }

        // Dimension 61..64: 1D, 2D, 3D, cube, 1D array, 2D array, -, cube array
        private static TexDim ATexDim(EmitterContext context, int dim)
        {
            return dim switch
            {
                0 => TexDim._1d,
                1 => TexDim._2d,
                2 => TexDim._3d,
                3 => TexDim.Cube,
                4 => TexDim.Array1d,
                5 => TexDim.Array2d,
                7 => TexDim.ArrayCube,
                _ => throw AFail(context, $"texture dimension {dim}"),
            };
        }

        // TMML: the LOD a sample would use, x and y in 8.8 fixed point, as Maxwell's
        public static void ATmml(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            EmitTmml(context, ATexDim(context, x[61..64]), AHandle(context, x), x[72..76], x.Ra, x.Rb, x.Rd, false);
        }

        // TXQ Rd, Ra: query 62..64 (0 the size at the LOD in Ra, 1 the type and sample count)
        public static void ATxq(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int query = x[62..64], handle = AHandle(context, x);
            SamplerType type = context.TranslatorContext.GpuAccessor.QuerySamplerType(handle);
            Instruction inst = query == 0 ? Instruction.TextureQuerySize : query == 1 ? Instruction.TextureQuerySamples : throw AFail(context, $"TXQ query {query}");
            SetBindingPair binding = context.ResourceManager.GetTextureOrImageBinding(inst, type, TextureFormat.Unknown, TextureFlags.None, TextureOperation.DefaultCbufSlot, handle);
            Operand[] sources = [context.Copy(AGpr(x.Ra))];
            int mask = x[72..76];
            Operand[] dests = ATexDests(x, BitOperations.PopCount((uint)mask), false);

            for (int comp = 0, i = 0; comp < 4; comp++)
            {
                if ((mask & (1 << comp)) != 0)
                {
                    Operand v = query == 0 ? context.TextureQuerySize(type, TextureFlags.None, binding, comp, sources)
                        : comp == 2 ? context.TextureQuerySamples(type, TextureFlags.None, binding, sources) : Const(0);
                    context.Copy(dests[i++], v);
                }
            }
        }

        // ---- surfaces ----

        // SULD Rd, [Ra] (0xb97 formatted, mask 72..76; 0xb99 binary, size 73..76) and SUST [Ra], Rb (0xb9b, 0xb9d): handle 40..59, dimension 61..64 (Maxwell's SuDim)
        public static void ASurface(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int handle = AHandle(context, x);
            bool formatted = x.Op is 0xb97 or 0xb9b;
            SuSize size = formatted ? 0 : (SuSize)x[73..76];
            SuRgba mask = formatted ? (SuRgba)x[72..76] : 0;

            if (x.Op is 0xb97 or 0xb99)
            {
                EmitSuld(context, CacheOpLd.Ca, (SuDim)x[61..64], size, handle, mask, x.Ra, x.Rd, 0, formatted, false, false);
            }
            else
            {
                EmitSust(context, CacheOpSt.Wb, (SuDim)x[61..64], size, handle, mask, x.Ra, x.Rb, 0, formatted, false, false);
            }
        }

        // SUATOM Rd, [Ra], Rb: as SUST's handle and dimension, ATOMG's operation (87..91) and size (73..76)
        public static void ASuatom(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            EmitSuatom(context, (SuDim)x[61..64], (SuatomOp)x[87..91], (SuatomSize)x[73..76], AHandle(context, x), x.Ra, x.Rb, 0, x.Rd, false, false, false);
        }
    }
}
