"""Run the production SSA interpolation rule against valid and near-match graphs."""
import html
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT/'nxnative/dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader/Ryujinx.Graphics.Shader.csproj'
HARNESS = r'''
using System;
using System.Linq;
using System.Reflection;

static class Check
{
    static readonly Assembly Shader = Assembly.Load("Ryujinx.Graphics.Shader");
    static Type T(string name) => Shader.GetType("Ryujinx.Graphics.Shader.IntermediateRepresentation." + name, true);
    static readonly Type Operand = T("Operand"), Operation = T("Operation"), Instruction = T("Instruction");
    static readonly MethodInfo Optimize = Shader.GetType("Ryujinx.Graphics.Shader.Translation.Optimizations.Optimizer", true)
        .GetMethod("EliminateMultiplyByFragmentCoordW", BindingFlags.NonPublic | BindingFlags.Static, null, new[] { Operation }, null);
    static object E(string type, string value) => Enum.Parse(T(type), value);
    static object Inst(string name, bool fp64 = false) => Enum.ToObject(Instruction,
        Convert.ToInt64(E("Instruction", name)) | Convert.ToInt64(E("Instruction", fp64 ? "FP64" : "FP32")));
    static object Local() => Activator.CreateInstance(Operand, E("OperandType", "LocalVariable"));
    static object Constant(int value) => Activator.CreateInstance(Operand, E("OperandType", "Constant"), value);
    static object Float(float value) => Constant(BitConverter.SingleToInt32Bits(value));
    static Array Sources(params object[] sources) { var a = Array.CreateInstance(Operand, sources.Length); for (int i = 0; i < sources.Length; i++) a.SetValue(sources[i], i); return a; }
    static object Load(string variable, int component, string storage = "Input")
    {
        object dest = Local();
        var sources = variable == "UserDefined" ? Sources(Constant(Convert.ToInt32(E("IoVariable", variable))), Constant(1), Constant(component))
            : Sources(Constant(Convert.ToInt32(E("IoVariable", variable))), Constant(component));
        Activator.CreateInstance(Operation, E("Instruction", "Load"), E("StorageKind", storage), dest, sources); return dest;
    }
    static object Math(string inst, object x, object y, bool fp64 = false)
    {
        object dest = Local(); Activator.CreateInstance(Operation, Inst(inst, fp64), dest, Sources(x, y)); return dest;
    }
    static object Assignment(object value) => Operand.GetProperty("AsgOp").GetValue(value);
    static void Require(bool value, string message) { if (!value) throw new Exception(message); }
    static void Case(bool swapped, float numerator = 1f, int component = 3, string inputStorage = "Input",
        bool outer64 = false, bool inner64 = false, bool reciprocal64 = false, bool extraFactor = false, bool innerSwapped = false)
    {
        object input = Load("UserDefined", 0, inputStorage);
        object attr = extraFactor ? Math("Multiply", input, Float(2)) : input;
        object w = Load("FragmentCoord", 3);
        object inner = innerSwapped ? Math("Multiply", w, attr, inner64) : Math("Multiply", attr, w, inner64);
        // A separate W load is legitimate: identity of the SSA temporary is not required.
        object reciprocal = Math("Divide", Float(numerator), Load("FragmentCoord", component), reciprocal64);
        object result = swapped ? Math("Multiply", reciprocal, inner, outer64) : Math("Multiply", inner, reciprocal, outer64);
        object operation = Assignment(result);
        bool expected = numerator == 1f && component == 3 && inputStorage == "Input" && !outer64 && !inner64 && !reciprocal64 && !extraFactor && !innerSwapped;
        Optimize.Invoke(null, new[] { operation });
        bool copied = Operation.GetProperty("Inst").GetValue(operation).Equals(E("Instruction", "Copy"));
        Require(copied == expected, "Unexpected match, reciprocal first=" + swapped + ", numerator=" + numerator + ", expected=" + expected);
        if (copied)
        {
            Require((int)Operation.GetProperty("SourcesCount").GetValue(operation) == 1, "Copy source count");
            Require(ReferenceEquals(Operation.GetMethod("GetSource").Invoke(operation, new object[] { 0 }), input), "Wrong interpolation input");
            foreach (object old in new[] { inner, reciprocal })
            {
                var uses = Operand.GetProperty("UseOps").GetValue(old);
                Require(!(bool)uses.GetType().GetMethod("Contains").Invoke(uses, new[] { operation }), "Stale SSA use");
            }
        }
    }
    public static void Main()
    {
        int cases = 0;
        foreach (bool swapped in new[] { false, true })
        {
            Case(swapped); cases++;
            foreach (float value in new[] { 0f, 2f, -1f, float.NaN, float.PositiveInfinity }) { Case(swapped, numerator:value); cases++; }
            foreach (int component in new[] { 0, 1, 2 }) { Case(swapped, component:component); cases++; }
            Case(swapped, inputStorage:"Output"); cases++;
            Case(swapped, outer64:true); cases++;
            Case(swapped, inner64:true); cases++;
            Case(swapped, reciprocal64:true); cases++;
            Case(swapped, extraFactor:true); cases++;
            Case(swapped, innerSwapped:true); cases++;
        }
        Console.WriteLine("perspective cancellation: PASS (" + cases + " SSA cases)");
    }
}
'''


class PerspectiveCancellationTests(unittest.TestCase):
    def test_production_ssa_rule(self):
        dotnet = os.environ.get('DOTNET') or shutil.which('dotnet')
        if not dotnet:
            self.skipTest('.NET SDK unavailable; set DOTNET to run the production SSA harness')
        with tempfile.TemporaryDirectory(prefix='nx2a_interp_') as directory:
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
            dll = work/'artifacts/bin/Check/release/Check.dll'
            result = subprocess.run([dotnet, str(dll)], capture_output=True, text=True, errors='replace', timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('perspective cancellation: PASS (30 SSA cases)', result.stdout)
