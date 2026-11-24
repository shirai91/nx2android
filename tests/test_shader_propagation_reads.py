"""Ryujinx's AstOptimizer propagates a variable into its use only when it is read once: x * x reads x twice from one parent node."""
import html
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_shader_perspective_cancellation import PROJECT, require_dotnet10

HARNESS = r'''
using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;

static class Check
{
    static readonly Assembly Shader = Assembly.Load("Ryujinx.Graphics.Shader");
    static Type T(string name) => Shader.GetType("Ryujinx.Graphics.Shader." + name, true);
    static readonly Type Node = T("StructuredIr.IAstNode"), Operand = T("StructuredIr.AstOperand"), Operation = T("StructuredIr.AstOperation"),
        Assignment = T("StructuredIr.AstAssignment"), Block = T("StructuredIr.AstBlock"), Inst = T("IntermediateRepresentation.Instruction"),
        OperandType = T("IntermediateRepresentation.OperandType"), Aggregate = T("Translation.AggregateType");
    static object E(Type t, string value) => Enum.Parse(t, value);
    static object Fp(string name) => Enum.ToObject(Inst, Convert.ToInt64(E(Inst, name)) | Convert.ToInt64(E(Inst, "FP32")));
    static object Const(int v) => Activator.CreateInstance(Operand, E(OperandType, "Constant"), v);
    static object Local() => Activator.CreateInstance(Operand, E(OperandType, "LocalVariable"), 0);
    static Array Nodes(params object[] n) { var a = Array.CreateInstance(Node, n.Length); for (int i = 0; i < n.Length; i++) a.SetValue(n[i], i); return a; }
    static object Op(string inst, params object[] sources) => Activator.CreateInstance(Operation, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic, null,
        new object[] { Fp(inst), Nodes(sources) }, null);
    static object Source(object op, int i) => Operation.GetMethod("GetSource").Invoke(op, new object[] { i });
    static int Count(object op) => (int)Operation.GetProperty("SourcesCount").GetValue(op);
    static void Require(bool value, string message) { if (!value) throw new Exception(message); }

    // Every node an expression reaches, counted at each visit (a shared subtree is visited again, as the SPIR-V generator emits it again)
    static long Visits(object node) { long n = 1; if (Operation.IsInstanceOfType(node)) for (int i = 0; i < Count(node); i++) n += Visits(Source(node, i)); return n; }

    static (object block, Dictionary<string, object> asg) Program(int squares)
    {
        object block = Activator.CreateInstance(Block, E(T("StructuredIr.AstBlockType"), "Main"), null);
        var asg = new Dictionary<string, object>();
        object Assign(string name, object src) { object dst = Local(); object a = Activator.CreateInstance(Assignment, dst, src); Block.GetMethod("Add").Invoke(block, new[] { a }); asg[name] = a; return dst; }
        // b is read once (b * 3): it is still propagated. x0 = b + 1, then x(i+1) = x(i) * x(i): each x is read twice by one parent
        object b = Assign("b", Op("Add", Const(1), Const(2)));
        object x = Assign("x0", Op("Multiply", b, Const(3)));
        for (int i = 1; i <= squares; i++) x = Assign("x" + i, Op("Multiply", x, x));
        Assign("out", Op("Add", x, Const(4)));
        return (block, asg);
    }

    static void Optimize(object block)
    {
        object fn = Activator.CreateInstance(T("StructuredIr.StructuredFunction"), BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic, null,
            new object[] { block, "main", E(Aggregate, "Void"), Array.CreateInstance(Aggregate, 0), Array.CreateInstance(Aggregate, 0) }, null);
        Type ctxType = T("StructuredIr.StructuredProgramContext");
        object ctx = RuntimeHelpers.GetUninitializedObject(ctxType);   // DebugMode false: the optimizer propagates
        ctxType.GetProperty("CurrentFunction").GetSetMethod(true).Invoke(ctx, new[] { fn });
        T("StructuredIr.AstOptimizer").GetMethod("Optimize", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic)
            .Invoke(null, new[] { ctx, E(T("Translation.TargetLanguage"), "Spirv") });
    }

    public static void Main()
    {
        const int squares = 40;   // 2^40 visits if the squares share their operand
        var (block, asg) = Program(squares);
        Optimize(block);
        var left = new HashSet<object>(); foreach (object n in (System.Collections.IEnumerable)block) left.Add(n);
        object Src(string name) => Assignment.GetProperty("Source").GetValue(asg[name]);
        Require(!left.Contains(asg["b"]), "b is read once: it must still be propagated into x0");
        Require(Operation.IsInstanceOfType(Source(Src("x0"), 0)), "x0 must read b's expression, not b");
        for (int i = 0; i < squares; i++) Require(left.Contains(asg["x" + i]), "x" + i + " is read twice (x * x): it must stay an assignment");
        for (int i = 1; i <= squares; i++)
        {
            object sq = Src("x" + i);
            Require(Operand.IsInstanceOfType(Source(sq, 0)) && ReferenceEquals(Source(sq, 0), Source(sq, 1)), "x" + i + " must square the variable x" + (i - 1));
        }
        long visits = 0; foreach (object n in left) visits += Visits(Assignment.GetProperty("Source").GetValue(n));
        Require(left.Count == squares, left.Count + " assignments left (b, x" + squares + " and the unused out go)");
        Require(visits < 1000, "the expressions reach " + visits + " nodes");
        Console.WriteLine("propagation reads: PASS (" + left.Count + " assignments, " + visits + " nodes)");
    }
}
'''


class PropagationReads(unittest.TestCase):
    def test_a_variable_read_twice_by_one_node_is_not_propagated(self):
        dotnet = os.environ.get('DOTNET') or shutil.which('dotnet')
        require_dotnet10(dotnet)
        with tempfile.TemporaryDirectory(prefix='nx2a_propagation_') as directory:
            work = Path(directory).resolve()   # macOS's temp folder is a link: MSBuild would miss the project references through it
            project = work/'Check.csproj'
            project.write_text('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType>'
                               '<TargetFramework>net10.0</TargetFramework><Nullable>disable</Nullable>'
                               '</PropertyGroup><ItemGroup><ProjectReference Include="' + html.escape(str(PROJECT), quote=True) +
                               '" /></ItemGroup></Project>', encoding='utf-8')
            (work/'Program.cs').write_text(HARNESS, encoding='utf-8')
            result = subprocess.run([dotnet, 'build', str(project), '-c', 'Release', '--artifacts-path', str(work/'artifacts'),
                                     '-p:NuGetAudit=false', '--nologo', '-v:q'], capture_output=True, text=True, errors='replace', timeout=180)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            # Before the fix the optimizer shared x(i-1)'s tree in both operands of every square: counting its nodes would never end, so a timeout is a failure too
            result = subprocess.run([dotnet, str(work/'artifacts/bin/Check/release/Check.dll')], capture_output=True, text=True, errors='replace', timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('propagation reads: PASS (40 assignments', result.stdout)


if __name__ == '__main__': unittest.main()
