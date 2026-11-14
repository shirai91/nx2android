import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tarfile
import tempfile
import types
import unittest
from unittest.mock import patch

SKILL = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SKILL / "scripts"))
import analyze
import gpu_profile
import profilelib
import runtime_integrate


def trace():
    return "\n".join(f"--- pass {i}: {w}x{h}, colour formats 37 9, depth format 126; c0=0:L({w}x{h})"
                      for i, (w, h) in enumerate(((32, 16), (64, 32))))


def full_log():
    lines = []
    for frame in (80, 100, 110, 120):
        lines += [f"replay up to present {frame}:",
                  "replay per frame (last 10): draws 8, replay 1.00 ms (descriptor sets 0.2), GPU busy 3.00 ms;"]
        for i, (w, h) in enumerate(((32, 16), (64, 32))):
            lines.append(f"GPU full pass row: render#{i} mean 1.000000 ms samples 10 size {w}x{h} depth_or_format 37 draws_or_z 1 coverage_width {w}")
        lines += ["GPU full pass row: dispatch#0 mean 0.500000 ms samples 1 size 2x1 depth_or_format 3349 draws_or_z 1 coverage_width 0",
                  "passes by GPU time (3 passes, 2.50 ms in passes per frame):",
                  "present GPU steps (10 game frames): initial transitions/clear 0.010000 ms, final image blit 0.020000 ms, overlay preparation/render 0.030000 ms, rotation/final transition 0.040000 ms; total 0.100000 ms"]
    return "\n".join(lines)


def range_log(mode=3, frame=20):
    count = 3
    phases = {1: [(x, count if x >= 6 else 0) for x in (0, 1, 2, 3, 4, 7)],
              2: [(x, count if x >= 6 else 0) for x in range(8)],
              3: [(0, 0), (7, count)],
              4: [(x, 0) for x in range(6)]+[(8, 2), (6, count), (7, count)],
              5: [(0, 0), (5, 0), (8, 2), (7, count)], 6: []}[mode]
    lines = [f"GPU range begin: capture 7 frame {frame} slot 1 mode {mode} markers {len(phases)} dropped 0 bits 36 period 1 step 2 first_draw 0 last_draw 2 draw_rows {count if mode >= 4 else 0}"]
    for q, (phase, draws) in enumerate(phases):
        lines.append(f"GPU range row: capture 7 frame {frame} slot 1 query {q} pass 9 phase {phase} part 2 64x32 draws {draws} stage {1 if phase == 0 else 8192} ticks {100+q*10} cpu_wall {1000+q*100} cpu_thread {1000+q*50}")
    if mode >= 4:
        for q in range(count):
            lines.append(f"GPU range draw: capture 7 frame {frame} slot 1 ordinal {q} pass 9 local_draw {q+1} global_draw {100+q} vs a fs b depth 1/1/4")
    lines.append(f"GPU range end: capture 7 frame {frame} slot 1")
    return "\n".join(lines)


def work_log(frame=20):
    lines = [f"shader workload begin: capture 99 frame {frame} slot 1 queries 3 dropped 0 (counts, not timings)"]
    for q in range(3):
        lines.append(f"shader workload row: capture 99 frame {frame} slot 1 query {q} pass 9 draw {101+q} 64x32 vs a fs b depth 1/1/4 IA 24/8 VS 12 CLIP 8/4 FS 16")
    lines.append(f"shader workload end: capture 99 frame {frame} slot 1")
    return "\n".join(lines)


class ParserTests(unittest.TestCase):
    def test_complete_windows_and_rare_compute(self):
        result = analyze.analyze_full(full_log(), analyze.parse_trace(trace()), 85, 120, 10, 3)
        self.assertEqual(len(result["windows"]), 3)
        self.assertEqual(result["reference_frames"], 30)
        self.assertAlmostEqual(result["render_ms"], 2)
        self.assertAlmostEqual(result["compute_ms"], .05)
        self.assertAlmostEqual(result["unassigned_ms"], .95)
        self.assertAlmostEqual(result["presentation"]["total_ms"], .1)

    def test_reject_changed_pass_and_incomplete_window(self):
        for bad in (full_log().replace("render#1", "render#3"),
                    full_log().replace("size 64x32", "size 128x64"),
                    full_log().replace("samples 10 size", "samples 1 size")):
            with self.assertRaises(profilelib.ProfileError):
                analyze.analyze_full(bad, analyze.parse_trace(trace()), 85, 120, 10, 3)

    def test_negative_residual_is_not_hidden(self):
        result = analyze.analyze_full(full_log().replace("GPU busy 3.00", "GPU busy 1.00"),
                                      analyze.parse_trace(trace()), 85, 120, 10, 3)
        self.assertLess(result["unassigned_ms"], 0)

    def test_every_range_mode_and_nonstandard_dimensions(self):
        for mode in range(1, 7):
            result = analyze.parse_range(range_log(mode))
            self.assertEqual(result["mode"], mode)
            self.assertEqual(result["passes"][0]["pass_"], 9)
            if mode >= 4:
                self.assertEqual(len(result["draws"]), 3)

    def test_range_rejects_bad_id_drop_phase_stage_and_part(self):
        original = range_log(4)
        for bad in (original.replace("dropped 0", "dropped 1"),
                    original.replace("query 1", "query 0", 1),
                    original.replace("phase 3", "phase 2", 1),
                    original.replace("stage 8192", "stage 1", 1),
                    original.replace("phase 1 part 2", "phase 1 part 3", 1),
                    original.replace("local_draw 2", "local_draw 1", 1),
                    original.replace("GPU range end:", "missing end:")):
            with self.assertRaises(profilelib.ProfileError):
                analyze.parse_range(bad)

    def test_same_frame_join_not_capture_serial(self):
        work = analyze.parse_work(work_log())
        census = analyze.parse_range(range_log(6))
        self.assertEqual(len(analyze.join_census(work, census)), 3)
        self.assertEqual(work["totals"]["fs_invocations"], 48)
        for wrong in (analyze.parse_work(work_log(21)), analyze.parse_work(work_log().replace("vs a", "vs c"))):
            with self.assertRaises(profilelib.ProfileError):
                analyze.join_census(wrong, census)

    def test_state_masks_generic_attachment_count(self):
        text = """=== traced pass 4: 32x16, colour formats 37 9, depth format 126
pipeline: topology 3;depth test 1 write 1 func 4
blend 0: src 0 dst 0 eq 0,write mask 0
draw: indexed draw; shaders a b 0; targets 1 (formats 37)
"""
        result = analyze.parse_state(text)
        self.assertTrue(result["all_color_disabled"])
        with self.assertRaises(profilelib.ProfileError):
            analyze.parse_state(text.replace("blend 0:", "other:"))


class GuardTests(unittest.TestCase):
    def test_packaged_runtime_asset_hashes(self):
        runtime = SKILL / "runtime"
        expected = json.loads((runtime / "assets.json").read_text(encoding="utf-8"))
        self.assertEqual(set(expected), {p.name for p in runtime.iterdir() if p.name != "assets.json"})
        for name, digest in expected.items():
            self.assertEqual(profilelib.sha((runtime/name).read_bytes()), digest, name)

    def test_timestamp_wrap_and_bad_properties(self):
        self.assertAlmostEqual(profilelib.delta_ticks(250, 5, 8, 1), 11e-6)
        for args in ((0, 1, 0, 1), (0, 1, 65, 1), (0, 1, 64, float("nan")), (100, 1, 36, 1)):
            with self.assertRaises(profilelib.ProfileError):
                profilelib.delta_ticks(*args)

    def test_history_tags_and_recent_slots(self):
        ns = [1_000_000]*64
        tags = [i+1 for i in range(64)]
        data = struct.pack("<128Q", *ns, *tags)
        completed, newest = profilelib.decode_history(data, 0, 512)
        self.assertEqual(newest, 63)
        self.assertEqual(len(completed), 56)
        tags[20] = 9999
        data = struct.pack("<128Q", *ns, *tags)
        self.assertNotIn(20, profilelib.decode_history(data, 0, 512)[0])
        with self.assertRaises(profilelib.ProfileError):
            profilelib.decode_history(b"short", 0, 512)

    def test_symbol_duplicate_different_size_and_mapped_gaps(self):
        text = "1 200 b g_frame_gpu\n2 8 b g_frame_gpu\n3 200 b g_frame_gpu_of\n4 8 b g_tex_frame\n5 68 B g_vkst\n"
        self.assertEqual(profilelib.symbol_offsets(text)["g_frame_gpu"], 1)
        self.assertTrue(profilelib.mapped_span([(0, 4), (4, 8)], 1, 7))
        self.assertFalse(profilelib.mapped_span([(0, 4), (5, 8)], 1, 7))
        with self.assertRaises(profilelib.ProfileError):
            profilelib.symbol_offsets(text + "9 200 b g_frame_gpu\n")

    def test_save_archive_rejects_traversal_and_links(self):
        def blob(name, link=False):
            stream = io.BytesIO()
            with tarfile.open(fileobj=stream, mode="w") as archive:
                entry = tarfile.TarInfo(name)
                if link:
                    entry.type = tarfile.SYMTYPE
                    entry.linkname = "/private"
                    archive.addfile(entry)
                else:
                    entry.size = 1
                    archive.addfile(entry, io.BytesIO(b"a"))
            return stream.getvalue()
        self.assertEqual(len(profilelib.archive_hashes(blob("files/save/a"))), 1)
        for data in (blob("../save"), blob("/save"), blob("files/save/a", True)):
            with self.assertRaises(profilelib.ProfileError):
                profilelib.archive_hashes(data)

    def test_output_containment(self):
        with tempfile.TemporaryDirectory() as root:
            self.assertTrue(profilelib.checked_output(root, Path(root)/".gamedump"/"App"/"run"))
            for path in (Path(root)/"outside", Path(root)/".gamedump", Path(root)/".gamedump"/"App"):
                with self.assertRaises(profilelib.ProfileError):
                    profilelib.checked_output(root, path)

    def test_controls_cannot_inject_lines_or_game_inputs(self):
        d = profilelib.Device(dict(adb="adb", serial="test", package="org.example.test"))
        with patch.object(d, "shell", return_value="") as shell:
            d.control({"DM_VK_PASS_SPLIT": "256"})
            self.assertIn("printf %s", shell.call_args.args[0])
            for values in ({"press": "A"}, {"DM_VK_A": "1\npress A"}):
                with self.assertRaises(profilelib.ProfileError):
                    d.control(values)


class CleanupTests(unittest.TestCase):
    def test_cleanup_preserves_original_failure_when_process_exits(self):
        device = types.SimpleNamespace(pid=lambda: (_ for _ in ()).throw(profilelib.ProfileError("app exited")))
        history = types.SimpleNamespace(pid="7")
        with tempfile.TemporaryDirectory() as directory:
            caught = None
            try:
                try:
                    raise profilelib.ProfileError("original capture error")
                finally:
                    gpu_profile.restore_live(device, history, Path(directory), {"DM_VK_RANGE_TIMES": 0})
            except profilelib.ProfileError as error:
                caught = error
            self.assertEqual(str(caught), "original capture error")
            self.assertIn("app exited", caught.__notes__[0])
            record = json.loads((Path(directory)/"cleanup.json").read_text())
            self.assertFalse(record["controls_restored"])

    def test_cleanup_rejects_new_process_without_writing_controls(self):
        calls = []
        device = types.SimpleNamespace(pid=lambda: "8", control=calls.append)
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(profilelib.ProfileError, "Process changed"):
                gpu_profile.restore_live(device, types.SimpleNamespace(pid="7"), Path(directory),
                                         {"DM_VK_RANGE_TIMES": 0})
            self.assertEqual(calls, [])

    def test_ab_failure_restores_explicit_baseline(self):
        class FakeDevice:
            calls = []
            def __init__(self, config): pass
            def blob(self, name): return b"normal"
            def sensors(self): raise profilelib.ProfileError("injected sensor failure")
            def pid(self): return "7"
            def control(self, values): self.calls.append(values)
            def verify(self): pass
            def focused(self): pass
        class FakeHistory:
            pid = "7"
            def __init__(self, device, output): pass
            def sample(self): return {"frame": 1}
        with tempfile.TemporaryDirectory() as directory:
            args = types.SimpleNamespace(positioned=True, key="DM_VK_PASS_SPLIT", baseline="256", candidate="512",
                pairs=1, seconds=2, warmup=0, exclude_frames=30, minimum_frames=1)
            config = dict(restore_controls={"DM_VK_PASS_SPLIT": "256"}, live_controls=["DM_VK_PASS_SPLIT"])
            with patch.object(gpu_profile, "Device", FakeDevice), patch.object(gpu_profile, "History", FakeHistory), \
                 patch.object(gpu_profile, "run_directory", return_value=Path(directory)):
                with self.assertRaisesRegex(profilelib.ProfileError, "injected sensor"):
                    gpu_profile.ab(config, args)
            self.assertEqual(FakeDevice.calls[-1]["DM_VK_PASS_SPLIT"], "256")

    def test_startup_control_rejected_for_live_ab(self):
        args = types.SimpleNamespace(positioned=True, key="DM_VK_DRIVER", baseline="a", candidate="b")
        with self.assertRaisesRegex(profilelib.ProfileError, "startup"):
            gpu_profile.ab(dict(live_controls=["DM_VK_DRIVER"]), args)

    def test_capture_failure_restores_live_diagnostics(self):
        class FakeDevice:
            calls = []
            files = "/app/files"
            def __init__(self, config): pass
            def blob(self, name): return b"normal"
            def sensors(self): return {}
            def pid(self): return "7"
            def control(self, values): self.calls.append(values)
            def verify(self): pass
        class FakeHistory:
            pid = "7"
            def __init__(self, device, output): pass
        with tempfile.TemporaryDirectory() as directory:
            args = types.SimpleNamespace(positioned=True, mode="range", frames=1,
                first=0, last=0, step=64, draw_first=0, draw_last=8, range_mode=3)
            with patch.object(gpu_profile, "Device", FakeDevice), patch.object(gpu_profile, "History", FakeHistory), \
                 patch.object(gpu_profile, "run_directory", return_value=Path(directory)), \
                 patch.object(gpu_profile, "await_capture", side_effect=profilelib.ProfileError("injected")):
                with self.assertRaisesRegex(profilelib.ProfileError, "injected"):
                    gpu_profile.capture({}, args)
            self.assertEqual(FakeDevice.calls[-1]["DM_VK_RANGE_TIMES"], 0)
            self.assertEqual(FakeDevice.calls[-1]["DM_VK_SHADER_WORK"], 0)
            self.assertEqual(FakeDevice.calls[-1]["DM_VK_TRACE_PASS"], -1)


class IntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo = SKILL.parents[2]
        cls.files = {}
        for name in ("gfx_vk.h", "gfx_vk_core.c", "gfx_vk_exec.c", "gfx_vk_present.c"):
            cls.files[name] = subprocess.check_output(["git", "show", "HEAD:nxnative/dm/android/"+name], cwd=cls.repo).decode()

    def test_clean_head_integration_and_idempotence(self):
        result = runtime_integrate.transform(self.files)
        self.assertIn(runtime_integrate.MARK, result["gfx_vk.h"])
        self.assertIn("GPU full pass row:", result["gfx_vk_core.c"])
        self.assertIn("present_gpu_submitted(k, 0)", result["gfx_vk_present.c"])
        self.assertEqual(result, runtime_integrate.transform(result))

    def test_drift_and_partial_integration_rejected(self):
        bad = dict(self.files)
        bad["gfx_vk_core.c"] = bad["gfx_vk_core.c"].replace("#define NSLOT 6", "#define NSLOT 9")
        with self.assertRaises(ValueError):
            runtime_integrate.transform(bad)
        bad = dict(self.files)
        bad["gfx_vk_core.c"] += "\nvk_sw_part();\n"
        with self.assertRaises(ValueError):
            runtime_integrate.transform(bad)


if __name__ == "__main__":
    unittest.main()
