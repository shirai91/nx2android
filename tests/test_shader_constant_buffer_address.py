"""Exercise production IR address simplification, including wrap and near matches."""
import html
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT/'nxnative/dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader/Ryujinx.Graphics.Shader.csproj'
HARNESS = r'''
using System;
using System.Collections.Generic;
using System.Reflection;

static class Check
{
    static readonly Assembly Shader = Assembly.Load("Ryujinx.Graphics.Shader");
    static Type T(string name) => Shader.GetType("Ryujinx.Graphics.Shader.IntermediateRepresentation." + name, true);
    static readonly Type Operand = T("Operand"), Operation = T("Operation"), Block = T("BasicBlock");
    static readonly MethodInfo Optimize = Shader.GetType("Ryujinx.Graphics.Shader.Translation.Optimizations.ConstantBufferAddress", true)
        .GetMethod("RunPass", BindingFlags.Public | BindingFlags.Static);
    static object E(string type, string value) => Enum.Parse(T(type), value);
    static object Local() => Activator.CreateInstance(Operand, E("OperandType", "LocalVariable"));
    static object Constant(uint value) => Activator.CreateInstance(Operand, E("OperandType", "Constant"), unchecked((int)value));
    static object Input() => Activator.CreateInstance(Operand, E("OperandType", "Argument"), 0);
    static object Field(object o, string name) => o.GetType().GetProperty(name).GetValue(o);
    static object Source(object op, int i) => Operation.GetMethod("GetSource").Invoke(op, new object[] { i });
    static object Assignment(object value) => Field(value, "AsgOp");
    static Array Sources(params object[] sources) { var a = Array.CreateInstance(Operand, sources.Length); for (int i = 0; i < sources.Length; i++) a.SetValue(sources[i], i); return a; }
    static object Math(object block, string name, object x, object y, bool fp = false)
    {
        object dest = Local();
        object inst = fp ? Enum.ToObject(T("Instruction"), Convert.ToInt64(E("Instruction", name)) | Convert.ToInt64(E("Instruction", "FP32"))) : E("Instruction", name);
        object op = Activator.CreateInstance(Operation, inst, dest, Sources(x, y));
        Block.GetMethod("Append").Invoke(block, new[] { op }); return dest;
    }
    static object Load(object block, object vector, object component, string storage = "ConstantBuffer")
    {
        object op = Activator.CreateInstance(Operation, E("Instruction", "Load"), E("StorageKind", storage), Local(), Sources(Constant(7), Constant(0), vector, component));
        Block.GetMethod("Append").Invoke(block, new[] { op }); return op;
    }
    static void Require(bool value, string message) { if (!value) throw new Exception(message); }
    static bool IsConstant(object value) => Field(value, "Type").Equals(E("OperandType", "Constant"));
    static Func<uint, uint> Evaluate(object value)
    {
        if (IsConstant(value)) { uint c = unchecked((uint)(int)Field(value, "Value")); return _ => c; }
        if (Field(value, "Type").Equals(E("OperandType", "Argument"))) return x => x;
        object op = Assignment(value); string name = Field(op, "Inst").ToString();
        Func<uint, uint> x = Evaluate(Source(op, 0)), y = Evaluate(Source(op, 1));
        return name switch
        {
            "Add" => v => unchecked(x(v) + y(v)),
            "Subtract" => v => unchecked(x(v) - y(v)),
            "Multiply" => v => unchecked(x(v) * y(v)),
            "BitwiseAnd" => v => x(v) & y(v),
            "BitwiseOr" => v => x(v) | y(v),
            "BitwiseExclusiveOr" => v => x(v) ^ y(v),
            "ShiftLeft" => v => x(v) << (int)y(v),
            "ShiftRightU32" => v => x(v) >> (int)y(v),
            "ShiftRightS32" => v => unchecked((uint)((int)x(v) >> (int)y(v))),
            _ when name.Contains("FP32") => v => unchecked((uint)BitConverter.SingleToInt32Bits(BitConverter.Int32BitsToSingle((int)x(v)) * BitConverter.Int32BitsToSingle((int)y(v)))),
            _ => throw new Exception("Unsupported evaluation " + name)
        };
    }
    static readonly uint[] Edges = { 0, 1, 2, 15, 16, 255, 256, 0x05555555, 0x0fffffff, 0x10000000, 0x55555555, 0x7fffffff, 0x80000000, 0xaaaaaaaau, 0xfffffff0, 0xffffffff };
    static int checkedInputs, cases;
    static void Verify(object block, object load, bool? expectConstant = null, bool untouched = false)
    {
        object oldV = Source(load, 2), oldC = Source(load, 3);
        var beforeV = Evaluate(oldV); var beforeC = Evaluate(oldC);
        Optimize.Invoke(null, new[] { block });
        object newV = Source(load, 2), newC = Source(load, 3);
        if (expectConstant.HasValue) Require(IsConstant(newC) == expectConstant.Value, "Unexpected component simplification");
        if (untouched) Require(ReferenceEquals(oldV, newV) && ReferenceEquals(oldC, newC), "Unexpected near-match rewrite");
        var afterV = Evaluate(newV); var afterC = Evaluate(newC);
        var random = new Random(18317 + cases++);
        for (int i = 0; i < 4096 + Edges.Length; i++)
        {
            uint v = i < Edges.Length ? Edges[i] : unchecked((uint)random.NextInt64(0, 0x100000000L));
            Require(beforeV(v) == afterV(v) && beforeC(v) == afterC(v), "Address mismatch at " + v.ToString("x8")); checkedInputs++;
        }
        foreach (var pair in new[] { (oldV, newV), (oldC, newC) })
        {
            if (ReferenceEquals(pair.Item1, pair.Item2)) continue;
            if (Field(pair.Item1, "Type").Equals(E("OperandType", "LocalVariable")))
            {
                object uses = Field(pair.Item1, "UseOps");
                Require(!(bool)uses.GetType().GetMethod("Contains").Invoke(uses, new[] { load }), "Stale replaced SSA use");
            }
        }
        // A second pass must not append another equivalent address calculation.
        int count = (int)Field(Field(block, "Operations"), "Count");
        Optimize.Invoke(null, new[] { block });
        Require(count == (int)Field(Field(block, "Operations"), "Count"), "Not idempotent");
    }
    static void Matrix(uint stride, uint offset, string shift = "ShiftRightU32", bool fp = false, string storage = "ConstantBuffer")
    {
        object b = Activator.CreateInstance(Block), input = Input();
        object address = Math(b, "Add", Math(b, "Multiply", input, Constant(stride), fp), Constant(offset));
        object word = Math(b, shift, address, Constant(2));
        object vector = Math(b, "ShiftRightU32", word, Constant(2));
        object component = Math(b, "BitwiseAnd", word, Constant(3));
        bool match = stride % 16 == 0 && shift == "ShiftRightU32" && !fp && storage == "ConstantBuffer";
        Verify(b, Load(b, vector, component, storage), expectConstant:match, untouched:!match);
    }
    static object Tree(object b, object input, Random r, int depth)
    {
        if (depth == 0) return r.Next(3) == 0 ? Constant(unchecked((uint)r.NextInt64(0, 0x100000000L))) : input;
        string[] names = { "Add", "Subtract", "Multiply", "BitwiseAnd", "BitwiseOr", "ShiftLeft", "ShiftRightU32", "ShiftRightS32" };
        string name = names[r.Next(names.Length)];
        object x = Tree(b, input, r, depth - 1);
        uint[] masks = { 0, 3, 15, 255, 0x0fffffff, 0x7fffffff, 0xffffffff, 0xfffffffc };
        object y = name.StartsWith("Shift") ? Constant((uint)r.Next(32)) : name == "BitwiseAnd" ? Constant(masks[r.Next(masks.Length)]) : Tree(b, input, r, depth - 1);
        return Math(b, name, x, y);
    }
    public static void Main()
    {
        foreach (uint stride in new uint[] { 16, 32, 48, 80, 112, 0x80000010, 0xfffffff0 })
            foreach (uint offset in new uint[] { 0, 4, 16, 20, 32, 44, 0xfffffffc }) Matrix(stride, offset);
        foreach (uint stride in new uint[] { 1, 3, 12, 20, 49 }) Matrix(stride, 4);
        Matrix(48, 32, shift:"ShiftRightS32"); Matrix(48, 32, fp:true); Matrix(48, 32, storage:"StorageBuffer");
        // Real lowering also increments the word address after its first shift.
        foreach (uint component in new uint[] { 1, 2, 3 })
        {
            object b = Activator.CreateInstance(Block), input = Input();
            object word = Math(b, "Add", Math(b, "ShiftRightU32", Math(b, "Multiply", input, Constant(48)), Constant(2)), Constant(component));
            Verify(b, Load(b, Math(b, "ShiftRightU32", word, Constant(2)), Math(b, "BitwiseAnd", word, Constant(3))), true);
        }
        // Preserve a carry across a previous mask and multiplication after truncation.
        foreach (string name in new[] { "Add", "Subtract", "Multiply" })
        {
            object b = Activator.CreateInstance(Block), input = Input();
            object value = Math(b, name, Math(b, "BitwiseAnd", input, Constant(255)), Constant(3));
            object vector = Math(b, "ShiftRightU32", Math(b, "ShiftLeft", value, Constant(4)), Constant(4));
            Verify(b, Load(b, vector, Math(b, "BitwiseAnd", Math(b, "ShiftLeft", input, Constant(2)), Constant(3))), true);
        }
        // Canonical expressions are shared within a block, never across control-flow boundaries.
        {
            object b = Activator.CreateInstance(Block), input = Input();
            object word = Math(b, "ShiftRightU32", Math(b, "Multiply", input, Constant(48)), Constant(2));
            object vector = Math(b, "ShiftRightU32", word, Constant(2));
            object lane = Math(b, "BitwiseAnd", word, Constant(3));
            object a = Load(b, vector, lane), c = Load(b, vector, lane);
            Verify(b, a, true); Require(ReferenceEquals(Source(a, 2), Source(c, 2)), "Address not shared");
        }
        var random = new Random(20261113);
        for (int i = 0; i < 256; i++)
        {
            object b = Activator.CreateInstance(Block), input = Input();
            object vector = Tree(b, input, random, 4);
            object lane = Math(b, "BitwiseAnd", Math(b, "ShiftLeft", input, Constant(2)), Constant(3));
            Verify(b, Load(b, vector, lane), true);
        }
        Console.WriteLine("constant buffer address: PASS (" + cases + " graphs, " + checkedInputs + " input checks)");
    }
}
'''


def require_dotnet10(dotnet):
    """Skip unsupported SDK installations before attempting a real shader build."""
    if not dotnet:
        raise unittest.SkipTest('.NET 10 SDK unavailable; set DOTNET to run the address harness')
    try:
        sdks = subprocess.run([dotnet, '--list-sdks'], capture_output=True, text=True,
                              check=True, timeout=30)
        roots = [Path(match.group(1)).parent for line in sdks.stdout.splitlines()
                 if (match := re.match(r'^10\.[^ ]+ \[(.+)\]$', line))]
        if not roots:
            raise unittest.SkipTest('.NET 10 SDK is required for the address harness')
        if not any(list((root / 'packs/Microsoft.NETCore.App.Ref').glob(
                '10.*/ref/net10.0/System.Runtime.dll')) for root in roots):
            raise unittest.SkipTest('.NET 10 reference assemblies are required')
        runtimes = subprocess.run([dotnet, '--list-runtimes'], capture_output=True,
                                  text=True, check=True, timeout=30)
        if not any(line.startswith('Microsoft.NETCore.App 10.')
                   for line in runtimes.stdout.splitlines()):
            raise unittest.SkipTest('.NET 10 runtime is required for the address harness')
    except (OSError, subprocess.SubprocessError) as error:
        raise unittest.SkipTest('Unable to query the .NET installation: ' + type(error).__name__) from error


class DotnetPrerequisitesTests(unittest.TestCase):
    def test_missing_executable_skips(self):
        with self.assertRaises(unittest.SkipTest):
            require_dotnet10(None)

    def test_older_sdk_skips_without_starting_a_build(self):
        result = subprocess.CompletedProcess([], 0, '8.0.408 [/sdk]\n')
        with mock.patch.object(subprocess, 'run', return_value=result) as run:
            with self.assertRaises(unittest.SkipTest):
                require_dotnet10('dotnet')
        self.assertEqual(run.call_count, 1)
        self.assertEqual(run.call_args.args[0][-1], '--list-sdks')

    def test_missing_reference_pack_skips(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.CompletedProcess([], 0, '10.0.100 [' + str(Path(directory)/'sdk') + ']\n')
            with mock.patch.object(subprocess, 'run', return_value=result):
                with self.assertRaises(unittest.SkipTest):
                    require_dotnet10('dotnet')

    def test_runtime_is_checked_after_the_reference_pack(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reference = root/'packs/Microsoft.NETCore.App.Ref/10.0.0/ref/net10.0/System.Runtime.dll'
            reference.parent.mkdir(parents=True)
            reference.touch()
            sdk = subprocess.CompletedProcess([], 0, '10.0.100 [' + str(root/'sdk') + ']\n')
            old = subprocess.CompletedProcess([], 0, 'Microsoft.NETCore.App 8.0.0 [/runtime]\n')
            valid = subprocess.CompletedProcess([], 0, 'Microsoft.NETCore.App 10.0.0 [/runtime]\n')
            with mock.patch.object(subprocess, 'run', side_effect=[sdk, old]):
                with self.assertRaises(unittest.SkipTest):
                    require_dotnet10('dotnet')
            with mock.patch.object(subprocess, 'run', side_effect=[sdk, valid]):
                require_dotnet10('dotnet')

    def test_unusable_installation_skips(self):
        with mock.patch.object(subprocess, 'run', side_effect=OSError('unavailable')):
            with self.assertRaises(unittest.SkipTest):
                require_dotnet10('dotnet')


class ConstantBufferAddressTests(unittest.TestCase):
    def test_production_address_rule(self):
        dotnet = os.environ.get('DOTNET') or shutil.which('dotnet')
        require_dotnet10(dotnet)
        with tempfile.TemporaryDirectory(prefix='nx2a_cbuf_') as directory:
            work = Path(directory)
            project = work/'Check.csproj'
            project.write_text('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType>'
                               '<TargetFramework>net10.0</TargetFramework><Nullable>disable</Nullable>'
                               '</PropertyGroup><ItemGroup><ProjectReference Include="' + html.escape(str(PROJECT), quote=True) +
                               '" /></ItemGroup></Project>', encoding='utf-8')
            (work/'Program.cs').write_text(HARNESS, encoding='utf-8')
            result = subprocess.run([dotnet, 'build', str(project), '-c', 'Release', '--artifacts-path', str(work/'artifacts'),
                                     '-p:NuGetAudit=false', '--nologo', '-v:q'], capture_output=True, text=True, errors='replace', timeout=180)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([dotnet, str(work/'artifacts/bin/Check/release/Check.dll')],
                                    capture_output=True, text=True, errors='replace', timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('constant buffer address: PASS', result.stdout)
