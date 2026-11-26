// One captured shader → its bundle entries, on the device: what dm/android/tools/shader_vk.py does for a run-time capture (--capture), around the same translator
// (dm/gfx/tools/dmsh_xlate/Program.cs, compiled in unchanged and run through its Main with a job file, as the porter runs it). The functions keep shader_vk.py's names and
// follow it line by line, so that a shader comes out byte for byte as the next port would bundle it (devtools/xlate_rt_check.py compares the two on a folder of captures).
// Not ported: android_shader_handles.tsv remaps (no profile with run-time translation has one: refused), the answer table (captures have no row there, as offline), and the
// fragment input mask of geometry passthrough, which offline is the union over the bundle's pixel shaders: the host passes it (gs_mask, default every location).
// C entry points (gfx_vk_shader_rt.c): dmsh_rt_translate, dmsh_rt_free. Blob: "DMRT", u32 count, count × {u64 key, u32 stage, u32 SPIR-V length, u32 reflection length},
// then each entry's SPIR-V and reflection (dmsv_refl, as the bundle's) in that order.
using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using System.Threading;

public static class DmshRuntime
{
    public sealed class Options { public int Subgroup = 128; public bool DynamicSlots; public int GsMask = 0xffff; }
    public sealed class Product { public ulong Key; public int Stage; public byte[] Spv, Refl; }

    const int SbAlign = 256, GatherBias = 8;   // shader_vk.py SB_ALIGN, GATHER_BIAS
    static readonly byte[] CtlMagic = { 0x34, 0x12, 0x76, 0x98 }, GfxMagic = { 0x78, 0x56, 0x34, 0x12 }, CsMagic = { 0x79, 0x56, 0x34, 0x12 };
    static readonly Dictionary<string, int> Stages = new() { ["vs"] = 0, ["ps"] = 1, ["gs"] = 2, ["hs"] = 3, ["ds"] = 4, ["cs"] = 5 };

    static uint U32(byte[] b, long o) => BitConverter.ToUInt32(b, (int)o);
    static bool Starts(byte[] b, byte[] m) => b.Length >= m.Length && b.AsSpan(0, m.Length).SequenceEqual(m);
    static byte[] Slice(byte[] b, long s, long e) { s = Math.Clamp(s, 0, b.Length); e = Math.Clamp(e, 0, b.Length); return e > s ? b[(int)s..(int)e] : Array.Empty<byte>(); }   // Python's b[s:e] for s, e >= 0
    static long PyMod(long a, long m) => ((a % m) + m) % m;

    // ---- shader_split.py sph_stage / glslc_stage ----
    static string SphStage(byte[] code)
    {
        uint w = U32(code, 0x30);
        if (((w >> 10) & 0xf) == 0 && (w & 0x1f) == 1) return "vs";
        return ((w >> 10) & 0xf) switch { 1 => "vs", 2 => "hs", 3 => "ds", 4 => "gs", 5 => "ps", _ => null };
    }
    static string GlslcStage(byte[] ctl, byte[] code)
    {
        bool headerlessCs = !Starts(code, CsMagic) && !Starts(code, GfxMagic) && ctl.Length >= 0x754 && U32(ctl, 0x748) != 0 && U32(ctl, 0x74c) != 0 && U32(ctl, 0x750) != 0;
        return Starts(code, CsMagic) || headerlessCs ? "cs" : Starts(code, GfxMagic) && code.Length >= 0x34 ? SphStage(code) : null;
    }

    // ---- shader_vk.py vertex_ab ----
    static (byte[] code, byte[] c1)? VertexAb(byte[] code, byte[] ctl)
    {
        if (code.Length < 0x80 || ctl.Length < 0x760 || !Starts(code, GfxMagic)) return null;
        uint w0 = U32(code, 0x30);
        if ((w0 & 0x1f) != 1 || ((w0 >> 10) & 0xf) != 0) return null;
        long aSph = U32(ctl, 0x748), aLen = U32(ctl, 0x74c), bSph = U32(ctl, 0x754), bLen = U32(ctl, 0x758);
        if (aSph == 0 && aLen == 0 && bSph == 0 && bLen == 0) return null;
        uint bw0 = 0x80 <= bSph && bSph <= code.Length - 0x50 ? U32(code, bSph) : 0;
        if (aSph != 0x30 || bSph != (long)U32(ctl, 0x700) + 0x30 || (bw0 & 0x1f) != 1 || ((bw0 >> 10) & 0xf) != 1 || PyMod(aLen - 0x50, 32) != 0 || PyMod(bLen - 0x50, 32) != 0 || bSph + bLen > code.Length)
            return null;
        uint[] sa = new uint[20], sb = new uint[20];
        for (int i = 0; i < 20; i++) { sa[i] = U32(code, aSph + 4 * i); sb[i] = U32(code, bSph + 4 * i); }
        uint[] sph = new uint[20];
        sph[0] = sb[0] | sa[0] & ~(0xfu << 10);
        for (int i = 1; i <= 3; i++) sph[i] = Math.Max(sa[i] & 0xffffff, sb[i] & 0xffffff) | sb[i] & ~0xffffffu;
        sph[4] = sb[4];
        for (int i = 5; i < 20; i++) sph[i] = sa[i] | sb[i];
        byte[] av = Slice(code, aSph + 0x50, aSph + aLen), b = Slice(code, bSph + 0x50, bSph + bLen);
        for (int o = 0; o < av.Length; o += 8)
        {
            if (o % 32 == 0) continue;
            if (o + 8 > av.Length) throw new InvalidOperationException("VertexA code is not whole instructions");   // struct.error in shader_vk.py
            ulong ins = BitConverter.ToUInt64(av, o);
            if (ins >> 52 == 0xe30 && (ins & 0x1f) == 0xf)
                BitConverter.TryWriteBytes(av.AsSpan(o), 0xe24UL << 52 | ((ulong)(av.Length - (o + 8)) & 0xffffff) << 20 | ins & 0xfffff);
        }
        var outp = new MemoryStream(); outp.Write(code, 0, 0x30);
        foreach (uint v in sph) outp.Write(BitConverter.GetBytes(v)); outp.Write(av); outp.Write(b);
        return (outp.ToArray(), Slice(code, bSph + bLen, code.Length));
    }

    // ---- shader_vk.py xfb_layout ----
    static JsonArray XfbLayout(string key, byte[] ctl, List<string> log)
    {
        ushort[] strides = new ushort[4]; for (int i = 0; i < 4; i++) strides[i] = BitConverter.ToUInt16(ctl, 0x6dc + 2 * i);
        if (strides.All(s => s == 0)) return null;
        var outp = new JsonArray();
        for (int b = 0; b < 4; b++)
        {
            if (strides[b] == 0) { outp.Add(null); continue; }
            int n = ctl[0x6e8 + b];
            byte[] outs = Slice(ctl, 0xdc + 0x80 * b, 0xdc + 0x80 * b + n), comps = Slice(ctl, 0x4dc + 0x80 * b, 0x4dc + 0x80 * b + n);
            var words = new List<int>();
            for (int i = 0; i < Math.Min(outs.Length, comps.Length); i++) for (int c = 0; c < comps[i]; c++) words.Add(0x20 + 4 * (outs[i] - 0x20) + c);
            if (n == 0 || outs.Any(o => o < 0x20 || o >= 0x40) || comps.Any(k => k < 1 || k > 4) || 4 * words.Count != strides[b])
            {
                log.Add($"[RED] {key}: transform feedback buffer {b} in a form not known (stride {strides[b]}): translated without capture");
                return null;
            }
            var wa = new JsonArray(); foreach (int w in words) wa.Add(JsonValue.Create(w));
            outp.Add(new JsonObject { ["stride"] = (int)strides[b], ["words"] = wa });
        }
        return outp;
    }

    static ulong GsVariantKey(ulong key, int t) => key ^ unchecked((ulong)(t + 1) * 0x9E3779B97F4A7C15UL);

    // ---- shader_vk.py job_for, for a capture (its record = 0x38-byte header, code area, control section) ----
    static (JsonObject job, byte[] c1) JobFor(string key, string stage, byte[] ctl, byte[] code0, string w, Options o, List<string> log)
    {
        byte[] code = code0, codeCtl = new byte[code0.Length + ctl.Length]; code0.CopyTo(codeCtl, 0); ctl.CopyTo(codeCtl, code0.Length);
        uint c1o = U32(ctl, 0x700), c1t = U32(ctl, 0x704);
        byte[] c1 = Slice(codeCtl, c1o, c1t);
        var ab = stage == "vs" ? VertexAb(code, ctl) : null;
        if (ab != null) (code, c1) = ab.Value;
        if (stage != "cs") code = Slice(code, 0x30, code.Length);
        else if (Starts(code, CsMagic)) code = Slice(code, 0x100, code.Length);
        string cp = Path.Combine(w, key + ".bin"); File.WriteAllBytes(cp, code);
        JsonArray xfb = stage == "vs" || stage == "gs" ? XfbLayout(key, ctl, log) : null;
        var j = new JsonObject
        {
            ["key"] = key, ["stage"] = stage, ["code"] = cp, ["tex"] = new JsonObject(), ["img"] = new JsonObject(), ["attr"] = new JsonObject(), ["fout"] = new JsonObject(),
            ["c1"] = Convert.ToHexString(c1).ToLowerInvariant(), ["debug"] = false, ["reduced"] = true, ["sb_align"] = SbAlign, ["subgroup"] = o.Subgroup, ["gather_bias"] = GatherBias,
            ["gs_passthrough"] = false, ["draw_parameters"] = true, ["viewport_mask"] = false, ["render_scale"] = false,
        };
        if (xfb != null) j["xfb"] = xfb;
        if (stage == "cs") { uint shared = U32(ctl, 0x754); j["local"] = new JsonArray((int)U32(ctl, 0x748), (int)U32(ctl, 0x74c), (int)U32(ctl, 0x750)); j["shared"] = (shared + 15) / 16 * 16; }
        if (stage == "ps" && ctl.Length >= 0x71c && (U32(ctl, 0x718) & 1) != 0) j["early_z"] = true;
        j["no_facts"] = true;   // a capture has no answer table row
        return (j, c1);
    }

    // ---- the translator: Program.cs's Main on a job file, as shader_vk.py xl runs it; its error lines are collected per thread ----
    sealed class ThreadText : TextWriter
    {
        readonly ThreadLocal<StringBuilder> _sb = new(() => new StringBuilder());
        public override Encoding Encoding => Encoding.UTF8;
        public override void Write(char c) => _sb.Value.Append(c);
        public override void Write(string s) => _sb.Value.Append(s);
        public string Take() { string s = _sb.Value.ToString(); _sb.Value.Clear(); return s; }
    }
    static ThreadText _err; static readonly object _consoleLock = new();
    [DynamicDependency("Main", typeof(SwshXlate))]
    static void Xlate(List<JsonObject> jobs, string w, List<string> log)
    {
        lock (_consoleLock) if (_err == null) { _err = new ThreadText(); Console.SetError(_err); Console.SetOut(TextWriter.Null); }
        string jp = Path.Combine(w, "job.json"); File.WriteAllText(jp, new JsonArray(jobs.Select(j => (JsonNode)j.DeepClone()).ToArray()).ToJsonString());
        var main = typeof(SwshXlate).GetMethod("Main", BindingFlags.NonPublic | BindingFlags.Static);
        try { main.Invoke(null, new object[] { new[] { jp, w } }); }
        catch (TargetInvocationException e) { log.Add($"[RED] translator: {e.InnerException?.GetType().Name}: {e.InnerException?.Message}"); }
        foreach (string ln in _err.Take().Split('\n')) if (ln.Trim().Length > 0) log.Add("translator: " + ln.Trim());
    }
    static (byte[] spv, JsonObject res)? Output(string w, string key)
    {
        string spv = Path.Combine(w, key + ".spv"), res = Path.Combine(w, key + ".res.json");
        if (!File.Exists(spv)) return null;
        byte[] data = File.ReadAllBytes(spv);
        if (data.Length < 20 || data.Length % 4 != 0 || U32(data, 0) != 0x07230203) throw new InvalidOperationException($"{key}: not SPIR-V");
        if (!File.Exists(res)) throw new InvalidOperationException($"{key}: translator omitted reflection");
        return (data, JsonNode.Parse(File.ReadAllText(res)).AsObject());
    }

    // ---- shader_vk.py spirv_bindings ----
    sealed class Ty { public string K; public uint[] F; public Ty(string k, params uint[] f) { K = k; F = f; } public uint this[int i] => F[i - 1]; public int Len => F.Length + 1; }
    public sealed class Bind
    {
        public string Name, Type, Kind; public uint Set, Binding, Count, Arrayed, Ms, Fmt; public uint? Dim;
        public long? Reg, Off, Samp, SampId; public long Bank; public long[] SampUbo; public bool SampTex, Rw, Fetch1d, HasBank;
    }
    static (List<Bind> binds, uint[] local, Dictionary<uint, uint[]> io) SpirvBindings(byte[] spv)
    {
        uint[] w = new uint[spv.Length / 4]; Buffer.BlockCopy(spv, 0, w, 0, w.Length * 4);
        var names = new Dictionary<uint, string>(); var deco = new Dictionary<uint, Dictionary<uint, uint>>(); var types = new Dictionary<uint, Ty>(); var consts = new Dictionary<uint, uint>();
        var vars = new List<(uint id, uint pt, uint sc)>();
        int i = 5;
        while (i < w.Length)
        {
            int n = (int)(w[i] >> 16); uint op = w[i] & 0xffff; if (n == 0) throw new InvalidOperationException("invalid SPIR-V instruction");
            uint A(int k) => w[i + k];
            if (op == 5) { var bytes = new byte[(n - 2) * 4]; Buffer.BlockCopy(w, (i + 2) * 4, bytes, 0, bytes.Length); int z = Array.IndexOf(bytes, (byte)0); names[A(1)] = Encoding.UTF8.GetString(bytes, 0, z < 0 ? bytes.Length : z); }
            else if (op == 71) { if (!deco.TryGetValue(A(1), out var d)) deco[A(1)] = d = new(); d[A(2)] = n > 3 ? A(3) : 1; }
            else if (op == 32) types[A(1)] = new Ty("ptr", A(2), A(3));
            else if (op == 28) types[A(1)] = new Ty("array", A(2), A(3));
            else if (op == 29) types[A(1)] = new Ty("rarray", A(2));
            else if (op == 25) types[A(1)] = new Ty("image", A(3), A(7), A(5), A(6), A(8));
            else if (op == 27) types[A(1)] = new Ty("sampled", A(2));
            else if (op == 26) types[A(1)] = new Ty("sampler");
            else if (op == 30) types[A(1)] = new Ty("struct");
            else if (op == 43 && n == 4) consts[A(2)] = A(3);
            else if (op == 59) vars.Add((A(2), A(1), A(3)));
            i += n;
        }
        var outp = new List<Bind>();
        foreach (var (vid, pt, sc) in vars)
        {
            var d = deco.GetValueOrDefault(vid) ?? new();
            if (!d.ContainsKey(33)) continue;
            uint? tid = types.ContainsKey(pt) ? types[pt][2] : null; Ty t = tid != null ? types.GetValueOrDefault(tid.Value) : null;
            uint count = 1;
            while (t != null && (t.K == "array" || t.K == "rarray")) { count = t.K == "array" ? consts.GetValueOrDefault(t[2], 1u) : 0; tid = t[1]; t = types.GetValueOrDefault(tid.Value); }
            string dt;
            if (sc == 2) dt = tid != null && deco.TryGetValue(tid.Value, out var td) && td.TryGetValue(3, out uint bb) && bb != 0 ? "ssbo" : "ubo";
            else if (sc == 12) dt = "ssbo";
            else if (t != null && t.K == "sampled") dt = types.TryGetValue(t[1], out var it) && it.Len > 1 && it[1] == 5 ? "tbuf" : "sampled";
            else if (t != null && t.K == "image") dt = t[1] == 5 && t[2] == 2 ? "storage_tbuf" : t[2] == 2 ? "storage" : t[1] == 5 ? "tbuf" : "image";
            else if (t != null && t.K == "sampler") dt = "sampler";
            else dt = "?";
            Ty img = t != null && t.K == "image" ? t : t != null && t.K == "sampled" ? types.GetValueOrDefault(t[1]) : null;
            outp.Add(new Bind { Name = names.GetValueOrDefault(vid, ""), Set = d.GetValueOrDefault(34u, 0u), Binding = d[33], Type = dt, Count = count,
                                Dim = img != null ? img[1] : null, Arrayed = img != null ? img[3] : 0, Ms = img != null ? img[4] : 0, Fmt = img != null ? img[5] : 0 });
        }
        uint[] local = null; var scalars = new Dictionary<uint, char>();
        for (i = 5; i < w.Length;)
        {
            int n = (int)(w[i] >> 16); uint op = w[i] & 0xffff;
            if (op == 16 && w[i + 2] == 17) local = new[] { w[i + 3], w[i + 4], w[i + 5] };
            else if (op == 21) scalars[w[i + 1]] = w[i + 3] != 0 ? 'i' : 'u';
            else if (op == 22) scalars[w[i + 1]] = 'f';
            else if (op == 23) scalars[w[i + 1]] = scalars.GetValueOrDefault(w[i + 2], 'f');
            i += n;
        }
        var io = new Dictionary<uint, uint[]> { [1] = new uint[3], [3] = new uint[3] };
        foreach (var (vid, pt, sc) in vars)
        {
            if (!io.ContainsKey(sc) || !(deco.GetValueOrDefault(vid)?.ContainsKey(30) ?? false)) continue;
            uint loc = deco[vid][30];
            if (loc >= 32) continue;
            Ty t = types.GetValueOrDefault(pt); uint? tid = t != null ? t[2] : null;
            while (tid != null && types.TryGetValue(tid.Value, out var tt) && (tt.K == "array" || tt.K == "rarray")) tid = tt[1];
            char k = tid != null ? scalars.GetValueOrDefault(tid.Value, 'f') : 'f';
            io[sc][0] |= 1u << (int)loc;
            if (k == 'i') io[sc][1] |= 1u << (int)loc;
            if (k == 'u') io[sc][2] |= 1u << (int)loc;
        }
        return (outp, local, io);
    }

    // ---- shader_products.py validate_bindings ----
    static List<JsonObject> Sbufs(JsonObject res) => res["sbufs"].AsArray().Select(x => x.AsObject()).OrderBy(x => (long)x["binding"]).ToList();   // stable, as sorted()
    static void ValidateBindings(List<Bind> binds, JsonObject res)
    {
        var sb = Sbufs(res);
        foreach (var b in binds)
        {
            var m = Regex.Match(b.Name, @"^[a-z]+_s(\d+)\z");
            if (m.Success) { int idx = int.Parse(m.Groups[1].Value); if (idx >= sb.Count || (long)sb[idx]["binding"] != b.Binding) throw new InvalidOperationException("storage buffer reflection does not match SPIR-V"); }
        }
    }

    // ---- shader_vk.py classify ----
    const int TexBase = 0x20, ImgBase = 0x120, ImgSlots = 8, SeptexBase = 0x168, SeptexSlot0 = 128, SampBase = 0x568;
    static readonly Dictionary<string, int> Ssbo = new() { ["vs"] = 0x110, ["gs"] = 0x410, ["cs"] = 0x310, ["ps"] = 0x510 };
    static long? Slot(long word, long bas, long cb) => cb == 2 && word * 4 >= bas && (word * 4 - bas) % 8 == 0 ? (word * 4 - bas) / 8 : null;
    static void Classify(string stage, Bind e, JsonObject res, Options o)
    {
        string nm = e.Name;
        if (o.DynamicSlots && (e.Type == "image" || e.Type == "sampler" || e.Type == "tbuf") && e.Count == 32 && Regex.IsMatch(nm, @"^[a-z]+_[ts]\w*_cb0_40000000\z"))
        { e.Kind = e.Type == "image" || e.Type == "tbuf" ? "texd" : "sampd"; return; }
        var m = Regex.Match(nm, @"^[a-z]+_c(\d+)\z");
        if (m.Success)
        {
            long n = long.Parse(m.Groups[1].Value);
            if (3 <= n && n <= 16) { e.Kind = "cbuf"; e.Reg = n - 3; } else { e.Kind = n == 1 ? "c1" : "drvcb"; e.Bank = n; e.HasBank = true; }
            return;
        }
        if (nm == "support_buffer") { e.Kind = "support"; return; }
        m = Regex.Match(nm, @"^[a-z]+_s(\d+)\z");
        if (m.Success)
        {
            int idx = int.Parse(m.Groups[1].Value); var sb = Sbufs(res);
            JsonObject s = idx < sb.Count ? sb[idx] : null; long? off = s != null && (long)s["cb_slot"] == 0 ? (long)s["cb_offset"] * 4 : null; long? bas = Ssbo.TryGetValue(stage, out int bv) ? bv : null;
            e.Kind = "sbuf"; e.Rw = s != null && ((long)s["flags"] & 1) != 0;
            if (stage == "cs" && off != null && 0x210 <= off && off < 0x310 && (off - 0x210) % 16 == 0) { e.Kind = "ubo_ptr"; e.Reg = (off - 0x210) / 16; }
            else if (s != null && (long)s["cb_slot"] >= 3) { e.Kind = "sbuf_ubo"; e.Reg = (long)s["cb_slot"] - 3; e.Off = (long)s["cb_offset"] * 4; }
            else e.Reg = off != null && bas != null && 0 <= off - bas && off - bas < 0x100 && (off - bas) % 16 == 0 ? (off - bas) / 16 : null;
            return;
        }
        m = Regex.Match(nm, @"^[a-z]+_([ti])\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)");
        if (m.Success)
        {
            string kind = m.Groups[1].Value; long slots = m.Groups[2].Success ? long.Parse(m.Groups[2].Value) : -1; long h = Convert.ToInt64(m.Groups[3].Value, 16);
            long tb = slots >= 0 ? slots & 0xffff : 2, sb = slots >= 0 && (slots >> 16) != 0 ? (slots >> 16) - 1 : tb;
            long o0 = h & 0x3fff, o1 = (h >> 14) & 0x3fff, ty = h >> 28;
            if (tb == 2)
            {
                var words = ty != 0 ? new[] { o0, o1 } : new[] { o0 };
                if (kind == "i")
                {
                    e.Kind = "img"; e.Reg = words.Where(x => ImgBase <= x * 4 && x * 4 < ImgBase + 8 * ImgSlots && Slot(x, ImgBase, tb) != null).Select(x => Slot(x, ImgBase, tb)).FirstOrDefault();
                    if (e.Type == "storage_tbuf") e.Kind = "img_tbuf";
                    return;
                }
                if (e.Count > 1 || e.Count == 0) { e.Kind = "tex_c2a"; e.Off = ty == 0 ? o0 * 4 : null; return; }
                var tw = ty == 3 ? new[] { o0 } : words;
                long? t = tw.Where(x => Slot(x, TexBase, tb) != null && x * 4 < SeptexBase).Select(x => Slot(x, TexBase, tb)).FirstOrDefault();
                if (t == null) t = tw.Where(x => Slot(x, SeptexBase, tb) != null && x * 4 < SampBase).Select(x => SeptexSlot0 + Slot(x, SeptexBase, tb)).FirstOrDefault();
                e.Kind = e.Type == "tbuf" ? "tbuf" : "tex"; e.Reg = t;
                if (ty == 3) e.SampId = o1;
                else if (ty != 0) e.Samp = words.Where(x => x * 4 >= SampBase && Slot(x, SampBase, sb) != null).Select(x => Slot(x, SampBase, sb)).FirstOrDefault();
                else e.SampTex = true;
                if (t != null && e.Kind == "tex" && res["fetch1d"] is JsonArray f1 && f1.Any(x => (long)x == t)) e.Fetch1d = true;
                return;
            }
            if (3 <= tb && tb <= 16 && (ty == 0 || ty == 1 || ty == 3) && !(ty == 1 && !(3 <= sb && sb <= 16)))
            {
                e.Kind = kind == "i" ? "img_ubo" : e.Type == "tbuf" ? "tbuf_ubo" : "tex_ubo"; e.Reg = tb - 3; e.Off = o0 * 4;
                if (ty == 3) e.SampId = o1;
                else if (ty == 1) e.SampUbo = new[] { sb - 3, o1 * 4 };
                else if (e.Kind == "tex_ubo") e.SampTex = true;
                return;
            }
        }
        e.Kind = "?";
    }

    // ---- shader_vk.py refl_bin ----
    static readonly Dictionary<string, int> Kind = new() { ["support"] = 0, ["cbuf"] = 1, ["c1"] = 2, ["drvcb"] = 3, ["sbuf"] = 4, ["ubo_ptr"] = 5, ["sbuf_ubo"] = 6, ["tex"] = 7, ["tbuf"] = 8,
        ["img"] = 9, ["img_tbuf"] = 10, ["tex_c2a"] = 11, ["tex_ubo"] = 12, ["tbuf_ubo"] = 13, ["img_ubo"] = 14, ["texd"] = 15, ["sampd"] = 16, ["?"] = 255 };
    static readonly Dictionary<string, int> Desc = new() { ["ubo"] = 0, ["ssbo"] = 1, ["sampled"] = 2, ["storage"] = 3, ["tbuf"] = 4, ["storage_tbuf"] = 5, ["image"] = 6, ["sampler"] = 7, ["?"] = 255 };
    static byte[] ReflBin(List<Bind> binds, byte[] c1, uint[] hdr3)
    {
        var o = new MemoryStream(); var bw = new BinaryWriter(o);
        bw.Write((uint)binds.Count); bw.Write((uint)c1.Length); foreach (uint v in hdr3.Concat(new uint[] { 1, 1, 1 }).Take(3)) bw.Write(v);
        foreach (var e in binds)
        {
            int kind = Kind.GetValueOrDefault(e.Kind, 255);
            long reg = e.Reg ?? -1; if (e.Kind == "drvcb") reg = e.HasBank ? e.Bank : 0;
            long off = e.Off ?? -1;
            int samp = 0; long sarg = 0;
            if (e.SampTex) samp = 1;
            else if (e.Samp != null) { samp = 2; sarg = e.Samp.Value; }
            else if (e.SampId != null) { samp = 3; sarg = e.SampId.Value; }
            else if (e.SampUbo != null) { samp = 4; sarg = e.SampUbo[0] | e.SampUbo[1] << 8; }
            int flags = (e.Arrayed != 0 ? 1 : 0) | (e.Ms != 0 ? 2 : 0) | (e.Rw ? 4 : 0) | (e.Fetch1d ? 8 : 0);
            bw.Write((byte)kind); bw.Write((byte)Desc.GetValueOrDefault(e.Type, 255)); bw.Write((byte)(e.Dim == null ? 0xff : e.Dim.Value)); bw.Write((byte)flags);
            bw.Write(checked((ushort)e.Binding)); bw.Write(checked((ushort)e.Count)); bw.Write(checked((short)reg)); bw.Write((byte)samp); bw.Write(checked((byte)e.Fmt)); bw.Write(checked((int)off)); bw.Write(checked((int)sarg));
        }
        bw.Write(c1); return o.ToArray();
    }

    // ---- shader_vk.py SPIRV_LITERALS / int_inputs ----
    static readonly Dictionary<uint, (int first, int count)> Literals = BuildLiterals();   // count -1: every word from first on (Python's None)
    static Dictionary<uint, (int, int)> BuildLiterals()
    {
        var d = new Dictionary<uint, (int, int)>();
        void Set(IEnumerable<int> ops, int f, int c) { foreach (int op in ops) d[(uint)op] = (f, c); }
        IEnumerable<int> R(int a, int b) => Enumerable.Range(a, b - a);
        Set(new[] { 2, 3, 4, 5, 6, 7, 8, 10, 11, 14, 15, 16, 17, 317, 330, 331 }, 0, -1); Set(R(19, 40), 0, -1); Set(R(41, 53), 0, -1);
        Set(new[] { 54, 59, 61, 65 }, 0, -1); Set(new[] { 71, 72, 73, 74, 75, 332, 5632, 5633 }, 0, -1);
        Set(new[] { 12 }, 3, 1); Set(new[] { 62, 63 }, 2, -1); Set(new[] { 64 }, 3, -1); Set(new[] { 68 }, 3, -1); Set(new[] { 79 }, 4, -1); Set(new[] { 81 }, 3, -1); Set(new[] { 82 }, 4, -1);
        Set(new[] { 87, 88, 91, 92, 95, 98, 305, 306, 309, 310, 313, 320 }, 4, 1); Set(new[] { 89, 90, 93, 94, 96, 97, 307, 308, 311, 312, 314, 315 }, 5, 1); Set(new[] { 99 }, 3, 1);
        Set(new[] { 246, 247 }, 0, -1); Set(new[] { 250 }, 3, -1); Set(new[] { 251 }, 1, -1);
        Set(R(264, 272).Append(342).Concat(R(349, 365)), 3, 1);
        return d;
    }
    static Dictionary<string, string> IntInputs(byte[] spv)
    {
        uint[] w = new uint[spv.Length / 4]; Buffer.BlockCopy(spv, 0, w, 0, w.Length * 4);
        var loc = new Dictionary<uint, uint>(); var inputs = new HashSet<uint>(); var chain = new Dictionary<uint, uint>(); var loads = new Dictionary<uint, uint>();
        var uses = new Dictionary<uint, int>(); var usesOrder = new List<uint>(); var other = new HashSet<uint>(); var signed = new Dictionary<uint, bool>(); var sint = new HashSet<uint>();
        var ins = new List<(uint op, uint[] a)>();
        for (int i = 5; i < w.Length;) { int n = (int)(w[i] >> 16); if (n == 0) break; ins.Add((w[i] & 0xffff, w[(i + 1)..Math.Min(i + n, w.Length)])); i += n; }
        foreach (var (op, a) in ins)
        {
            if (op == 71 && a.Length > 2 && a[1] == 30) loc[a[0]] = a[2];
            else if (op == 59 && a[2] == 1) inputs.Add(a[1]);
            else if (op == 21) signed[a[0]] = a[2] != 0;
            else if (op == 23 && signed.ContainsKey(a[1])) signed[a[0]] = signed[a[1]];
        }
        foreach (var (op, a) in ins) if (op == 65 && inputs.Contains(a[2])) chain[a[1]] = a[2];
        foreach (var (op, a) in ins) if (op == 61 && (inputs.Contains(a[2]) || chain.ContainsKey(a[2]))) loads[a[1]] = inputs.Contains(a[2]) ? a[2] : chain[a[2]];
        foreach (var (op, a) in ins)
        {
            IEnumerable<uint> ops;
            if (op == 124) ops = a.Skip(2);
            else { var (first, count) = Literals.TryGetValue(op, out var fc) ? fc : (a.Length, 0); ops = a.Take(first).Concat(count < 0 ? Enumerable.Empty<uint>() : a.Skip(first + count)); }
            foreach (uint x in ops)
            {
                if (!loads.TryGetValue(x, out uint v)) continue;
                if (op == 124) { if (!uses.ContainsKey(v)) usesOrder.Add(v); uses[v] = uses.GetValueOrDefault(v) + 1; if (signed.GetValueOrDefault(a[0])) sint.Add(v); }
                else other.Add(v);
            }
        }
        var r = new Dictionary<string, string>();
        foreach (uint v in usesOrder) if (!other.Contains(v) && loc.ContainsKey(v)) r[loc[v].ToString()] = sint.Contains(v) ? "sint" : "uint";
        return r;
    }

    // ---- shader_vk.py main, for one capture ----
    public static ulong KeyOf(byte[] ctl, byte[] code)
    {
        byte[] h = SHA256.HashData(ctl.Concat(code).ToArray()); ulong k = 0; for (int i = 0; i < 8; i++) k = k << 8 | h[i]; return k;
    }
    public static List<Product> TranslateCapture(byte[] ctl, byte[] code, Options o, string w, List<string> log)
    {
        if (ctl.Length < 0x760 || !Starts(ctl, CtlMagic)) throw new InvalidOperationException("not a control section");
        if (code.AsSpan().IndexOf(CtlMagic) >= 0) throw new InvalidOperationException("the control magic appears in its code area");   // shader_vk.py captured: job_for splits there
        ulong key64 = KeyOf(ctl, code); string k = key64.ToString("x16");
        string st = GlslcStage(ctl, code) ?? throw new InvalidOperationException("cannot determine its shader stage");
        Directory.CreateDirectory(w);
        var jobs = new Dictionary<string, JsonObject>(); var keys = new List<string> { k }; var gsVar = new Dictionary<string, ulong>();
        var (job, c1) = JobFor(k, st, ctl, code, w, o, log); jobs[k] = job;
        if (st == "gs")
            foreach (int t in new[] { 0, 1, 2, 4 })
            {
                string vk = $"{k}_t{t}"; var vj = (JsonObject)job.DeepClone(); vj["key"] = vk; vj["gs_input"] = t; jobs[vk] = vj; keys.Add(vk); gsVar[vk] = GsVariantKey(key64, t);
            }
        Xlate(keys.Select(x => jobs[x]).ToList(), w, log);
        var failures = new Dictionary<string, string>();
        // second pass: a vertex shader's inputs used only as integers, declared so
        if (st == "vs")
            try
            {
                var p = Output(w, k);
                if (p != null)
                {
                    var attr = jobs[k]["attr"].AsObject(); var extra = IntInputs(p.Value.spv).Where(kv => !attr.ContainsKey(kv.Key)).ToList();
                    if (extra.Count > 0)
                    {
                        foreach (var kv in extra) attr[kv.Key] = kv.Value;
                        foreach (string sfx in new[] { ".spv", ".res.json" }) File.Delete(Path.Combine(w, k + sfx));
                        Xlate(new List<JsonObject> { jobs[k] }, w, log);
                    }
                }
            }
            catch (Exception e) { failures[k] = e.Message; }
        // geometry passthrough with the host's fragment input mask (offline: the bundle's pixel shaders')
        var geometry = keys.Where(x => st == "gs" && jobs[x]["xfb"] == null && !failures.ContainsKey(x)).ToList();
        foreach (string g in geometry)
        {
            jobs[g]["gs_passthrough_mask"] = o.GsMask & 0xffff;
            foreach (string sfx in new[] { ".spv", ".res.json" }) File.Delete(Path.Combine(w, g + sfx));
        }
        if (geometry.Count > 0) Xlate(geometry.Select(x => jobs[x]).ToList(), w, log);
        var outp = new List<Product>();
        foreach (string x in keys)
        {
            ulong ek = gsVar.TryGetValue(x, out ulong gv) ? gv : key64;
            try
            {
                if (failures.TryGetValue(x, out string f)) throw new InvalidOperationException(f);
                var p = Output(w, x) ?? throw new InvalidOperationException("no output");
                var (spv, res) = p;
                var (binds, local, io) = SpirvBindings(spv);
                ValidateBindings(binds, res);
                foreach (var b in binds) Classify(st, b, res, o);
                uint[] hdr3 = st == "cs" ? local ?? (job["local"] is JsonArray la ? la.Select(v => (uint)(int)v).ToArray() : new uint[] { 1, 1, 1 }) : st == "vs" ? io[1] : st == "ps" ? io[3] : new uint[3];
                outp.Add(new Product { Key = ek, Stage = Stages[st], Spv = spv, Refl = ReflBin(binds, c1, hdr3) });
            }
            catch (Exception e) { log.Add($"[RED] {ek:x16}: {e.Message}"); }
        }
        return outp;
    }

    public static byte[] Blob(List<Product> ps)
    {
        var o = new MemoryStream(); var bw = new BinaryWriter(o);
        bw.Write(0x54524d44u); bw.Write((uint)ps.Count);   // "DMRT"
        foreach (var p in ps) { bw.Write(p.Key); bw.Write((uint)p.Stage); bw.Write((uint)p.Spv.Length); bw.Write((uint)p.Refl.Length); }
        foreach (var p in ps) { bw.Write(p.Spv); bw.Write(p.Refl); }
        return o.ToArray();
    }

#if DMSH_RT_NATIVE
    // int dmsh_rt_translate(const uint8_t *ctl, uint32_t nctl, const uint8_t *code, uint32_t ncode, const char *opts, const char *workdir, uint8_t **blob, uint32_t *nblob, char *log, uint32_t nlog)
    // opts: "subgroup=128 dynamic_slots=0 gs_mask=65535" (any may be left out). 0 with a blob (free it with dmsh_rt_free; it may hold fewer entries than asked, each
    // failure in log), 1 with nothing. log: the translator's and this file's messages, NUL-terminated, cut to nlog
    [UnmanagedCallersOnly(EntryPoint = "dmsh_rt_translate")]
    public static unsafe int Translate(byte* ctl, uint nctl, byte* code, uint ncode, byte* opts, byte* workdir, byte** blob, uint* nblob, byte* logbuf, uint nlog)
    {
        var log = new List<string>(); int rc = 1; *blob = null; *nblob = 0;
        try
        {
            var o = new Options();
            foreach (string kv in (Marshal.PtrToStringUTF8((IntPtr)opts) ?? "").Split(' ', StringSplitOptions.RemoveEmptyEntries))
            {
                string[] p = kv.Split('=', 2); if (p.Length != 2) continue;
                if (p[0] == "subgroup") o.Subgroup = int.Parse(p[1]); else if (p[0] == "dynamic_slots") o.DynamicSlots = p[1] != "0"; else if (p[0] == "gs_mask") o.GsMask = int.Parse(p[1]);
            }
            string w = Path.Combine(Marshal.PtrToStringUTF8((IntPtr)workdir) ?? ".", $"t{Environment.CurrentManagedThreadId}");
            var ps = TranslateCapture(new ReadOnlySpan<byte>(ctl, (int)nctl).ToArray(), new ReadOnlySpan<byte>(code, (int)ncode).ToArray(), o, w, log);
            try { Directory.Delete(w, true); } catch (Exception) { }
            if (ps.Count > 0)
            {
                byte[] b = Blob(ps); byte* m = (byte*)NativeMemory.Alloc((nuint)b.Length); b.CopyTo(new Span<byte>(m, b.Length)); *blob = m; *nblob = (uint)b.Length; rc = 0;
            }
        }
        catch (Exception e) { log.Add($"[RED] {e.GetType().Name}: {e.Message}"); }
        if (logbuf != null && nlog > 0)
        {
            byte[] t = Encoding.UTF8.GetBytes(string.Join("\n", log)); int n = Math.Min(t.Length, (int)nlog - 1);
            t.AsSpan(0, n).CopyTo(new Span<byte>(logbuf, n)); logbuf[n] = 0;
        }
        return rc;
    }
    [UnmanagedCallersOnly(EntryPoint = "dmsh_rt_free")]
    public static unsafe void Free(byte* p) => NativeMemory.Free(p);
#endif
}
