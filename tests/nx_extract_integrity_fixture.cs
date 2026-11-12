// Real LibHac storage and synthetic containers under the production error boundary; no keys or game data.
using System;
using System.IO;
using System.Reflection;
using System.Runtime.ExceptionServices;
using LibHac;
using LibHac.Fs;
using LibHac.Common.Keys;
using LibHac.FsSystem;
using LibHac.Tools.FsSystem;
using LibHac.Tools.FsSystem.RomFs;
using Path = System.IO.Path;
using CheckedStorage = LibHac.Tools.FsSystem.IntegrityVerificationStorage;

static class IntegrityFixture {
    const BindingFlags Hidden = BindingFlags.Static | BindingFlags.NonPublic;
    static object Call(string name, params object[] args) {
        try { return typeof(P).GetMethod(name, Hidden).Invoke(null, args); }
        catch (TargetInvocationException e) { ExceptionDispatchInfo.Capture(e.InnerException).Throw(); throw; }
    }
    static void Set(string name, object value) => typeof(P).GetField(name, Hidden).SetValue(null, value);
    static int Boundary(Func<int> body) {
        // Before the fix there is no boundary: the same LibHac failure escapes to the process.
        return typeof(P).GetMethod("Run", Hidden) == null ? body() : (int)Call("Run", body);
    }
    static CheckedStorage Checked(byte[] bytes, byte[] hashes, int block = 512) {
        var info = new IntegrityVerificationInfo { Data = new MemoryStorage(bytes), BlockSize = block, Type = IntegrityStorageType.RomFs };
        return new CheckedStorage(info, new MemoryStorage(hashes), IntegrityCheckLevel.ErrorOnInvalid, false);
    }
    static byte[] Payload() { var b = new byte[8192]; for (int i = 0; i < b.Length; i++) b[i] = (byte)(i * 17 + 3); return b; }
    static int CopyCase(string mode, string output) {
        var builder = new RomFsBuilder();
        builder.AddFile("/sample.bin", new StorageFile(new MemoryStorage(Payload()), OpenMode.Read));
        using var image = builder.Build(); image.GetSize(out long size).ThrowIfFailure();
        var raw = new byte[(size + 511) & ~511L]; image.Read(0, raw.AsSpan(0, (int)size)).ThrowIfFailure();
        var hashes = new byte[raw.Length / 512 * 32];
        using (var writer = Checked(raw, hashes))
            for (int i = 0; i < raw.Length; i += 512) writer.Write(i, raw.AsSpan(i, 512)).ThrowIfFailure();
        if (mode != "copy") raw[BitConverter.ToInt64(raw, 0x48) + 4096] ^= 1; // file data only; header and directory tables remain verified
        Set("g_allow", mode == "copy-allow");
        Set("g_lenient", (Func<LibHac.Fs.Fsa.IFileSystem>)(() => new RomFsFileSystem(new MemoryStorage(raw))));
        using var storage = Checked(raw, hashes); using var cache = new CachedStorage(storage, 4, true);
        using var fs = new RomFsFileSystem(cache);
        Call("Copy", fs, output, null);
        return (int)Call("Damaged");
    }
    static int NcaCase(string mode, string output) {
        // An ordinary PFS0 around artificial bytes: reproduce LibHac's actual header rejection through OpenNsp.
        string input = Path.Combine(Path.GetDirectoryName(output), mode + " container's sample.nsp");
        byte[] name = System.Text.Encoding.ASCII.GetBytes("header-entry.nca\0"), header = new byte[0xc00];
        if (mode == "nca-readable") System.Text.Encoding.ASCII.GetBytes("NCA3").CopyTo(header, 0x200); // a readable, unencrypted header; no filesystem is opened
        using (var w = new BinaryWriter(File.Create(input))) {
            w.Write(System.Text.Encoding.ASCII.GetBytes("PFS0")); w.Write(1); w.Write(name.Length); w.Write(0);
            w.Write(0L); w.Write((long)header.Length); w.Write(0); w.Write(0); w.Write(name); w.Write(header);
        }
        Set("g_allow", mode == "nca-allow");
        var ncas = (System.Collections.ICollection)Call("OpenNsp", new KeySet(), input);
        return ncas.Count == 1 ? 0 : 92;
    }
    static int Main(string[] args) {
        try {
            var mode = args[0];
            if (mode == "usage") return (int)Call("Main", (object)Array.Empty<string>());
            return Boundary(() => {
                if (mode.StartsWith("copy")) return CopyCase(mode, args[1]);
                if (mode.StartsWith("nca")) return NcaCase(mode, args[1]);
                if (mode == "invalid-data") throw new InvalidDataException("unrelated parser sentinel");
                if (mode == "io") throw new IOException("file access sentinel");
                if (mode == "other-result") throw new LibHac.Common.HorizonResultException(new Result(2, 1));
                if (mode == "other-module") throw new LibHac.Common.HorizonResultException(new Result(3, 4604));
                if (mode == "outside-range") throw new LibHac.Common.HorizonResultException(new Result(2, 4700));
                Set("g_allow", mode == "fatal-allow");
                var hashes = new byte[32]; Array.Fill(hashes, (byte)1);
                using var storage = Checked(new byte[4096], hashes, 4096);
                using var cache = new CachedStorage(storage, 4, true);
                using var fs = new RomFsFileSystem(cache); // raises the real 2002-4604 while reading RomfsHeader
                return 0;
            });
        } catch (Exception e) {
            Console.Error.WriteLine("UNHANDLED " + e.GetType().FullName + ": " + e.Message);
            return 91; // make propagation observable without generating a core dump
        }
    }
}
