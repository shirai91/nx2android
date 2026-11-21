#!/usr/bin/env python3
"""Explicit, bounded CLI for reusable nx2android Vulkan performance investigations."""
import argparse
from pathlib import Path
import re
import shlex
import sys
import time

from profilelib import (Device, History, ProfileError, archive_hashes, checked_output,
                        initialize, load_config, parse_env, require, sha, stats, write_json)
from analyze import (analyze_full, join_census, parse_range, parse_state, parse_trace,
                     parse_work, write_full_report)


def run_directory(config, kind):
    path = Path(config["output"]) / (kind + "-" + time.strftime("%Y%m%d-%H%M%S"))
    path.mkdir(parents=True, exist_ok=False)
    return path


def restore_live(device, history, output, controls, env=None, settings=None, hardware=False):
    """Attempt independent cleanup checks without hiding the capture's original error."""
    primary = sys.exception()
    errors = []
    restored = False
    try:
        require(device.pid() == history.pid, "Process changed; live controls were not sent to a new process")
        device.control(controls)
        restored = True
    except Exception as error:
        errors.append(f"Live controls: {error}")
    if env is not None:
        try:
            require(device.blob("env.txt") == env and device.blob("settings.txt") == settings,
                    "Persistent configuration changed")
            device.verify()
        except Exception as error:
            errors.append(f"Configuration/identity: {error}")
    if hardware:
        try:
            write_json(output / "hardware-after.json", device.sensors())
        except Exception as error:
            errors.append(f"Hardware snapshot: {error}")
    try:
        write_json(output / "cleanup.json", dict(controls_restored=restored, errors=errors))
    except Exception as error:
        errors.append(f"Cleanup record: {error}")
    if errors:
        message = "Cleanup incomplete: " + "; ".join(errors)
        if primary is not None:
            primary.add_note(message)
        else:
            raise ProfileError(message)


def await_capture(device, history, output, controls, end_tokens, filename):
    offset = device.log_offset()
    device.control(controls)
    for _ in range(50):
        time.sleep(.2)
        require(device.pid() == history.pid, "Process changed during bounded capture")
        text = device.log_since(offset)
        require("GPU range unavailable:" not in text and "shader workload unavailable:" not in text,
                "Diagnostic unavailable; inspect saved runtime capability before retrying")
        if all(token in text for token in end_tokens):
            (output / filename).write_text(text, encoding="utf-8")
            return text
    raise ProfileError("Bounded diagnostic did not complete; do not install/restart automatically")


def trace_once(device, history, output, ordinal=-2):
    before = history.sample()["frame"]
    offset = device.log_offset()
    device.control({"DM_VK_TRACE_PASS": ordinal})
    try:
        for _ in range(20):
            time.sleep(.15)
            if history.sample()["frame"] > before+6:
                break
        else:
            raise ProfileError("Frame did not advance during state trace")
    finally:
        restore_live(device, history, output, {"DM_VK_TRACE_PASS": -1})
    text = device.log_since(offset)
    (output / ("passes.log" if ordinal == -2 else f"state-{ordinal}.log")).write_text(text, encoding="utf-8")
    return text


def capture(config, args):
    require(args.positioned, "Confirm user has positioned the scene before --positioned")
    d = Device(config)
    output = run_directory(config, args.mode)
    h = History(d, output)
    env, settings = d.blob("env.txt"), d.blob("settings.txt")
    (output / "env.txt").write_bytes(env)
    (output / "settings.txt").write_bytes(settings)
    write_json(output / "hardware-before.json", d.sensors())
    result = None
    try:
        if args.mode in ("full", "baseline"):
            passes = None
            if args.mode == "full":
                parsed = parse_env(env)
                require(parsed.get("DM_VK_PASS_TIMES") == parsed.get("DM_VK_PASS_FULL") == "1",
                        "Complete timers require an approved startup cycle; use timing-plan first")
                passes = parse_trace(trace_once(d, h, output))
                write_json(output / "passes.json", passes)
            start = h.sample()
            anchor, began = start["frame"], time.monotonic()
            samples, accepted = [], {}
            while time.monotonic()-began < args.seconds:
                tick = time.monotonic()
                d.focused()
                row = h.sample()
                row["elapsed"] = tick-began
                if len(samples) % 5 == 0:
                    row["sensors"] = d.sensors()
                samples.append(row)
                accepted.update({f: v for f, v in row["completed"].items() if f > anchor+30})
                write_json(output / "samples.json", samples)
                if len(samples) % 10 == 0:
                    print(f"Collected {int(time.monotonic()-began)} seconds", flush=True)
                time.sleep(max(0, 1.5-(time.monotonic()-tick)))
            last = h.sample()
            require(accepted, "No stable positioned history")
            result = dict(pid=h.pid, anchor_frame=anchor, end_frame=last["frame"],
                          seconds=time.monotonic()-began, history=stats(list(accepted.values())),
                          replay_ms=(last["replay_ns"]-start["replay_ns"])/(last["frame"]-anchor)/1e6)
            write_json(output / "manifest.json", result)
            log = d.blob("log.txt").decode(errors="replace")
            (output / "log.txt").write_text(log, encoding="utf-8")
            if passes:
                result["analysis"] = analyze_full(log, passes, anchor, last["frame"], args.report_period, args.minimum_windows)
                write_json(output / "analysis.json", result["analysis"])
                write_full_report(output, result["analysis"])
        elif args.mode == "state":
            result = []
            for p in range(args.first, args.last+1):
                result.append(parse_state(trace_once(d, h, output, p)))
                write_json(output / "states.json", result)
        else:
            result = []
            for i in range(args.frames):
                controls = dict(DM_VK_RANGE_FIRST=args.first, DM_VK_RANGE_LAST=args.last,
                                DM_VK_RANGE_DRAW_STEP=args.step, DM_VK_RANGE_DRAW_FIRST=args.draw_first,
                                DM_VK_RANGE_DRAW_LAST=args.draw_last, DM_VK_SHADER_WORK_FIRST=args.first,
                                DM_VK_SHADER_WORK_LAST=args.last)
                tokens = []
                if args.mode in ("range", "joined"):
                    controls["DM_VK_RANGE_TIMES"] = 6 if args.mode == "joined" else args.range_mode
                    tokens.append("GPU range end:")
                if args.mode in ("work", "joined"):
                    controls["DM_VK_SHADER_WORK"] = 1
                    tokens.append("shader workload end:")
                text = await_capture(d, h, output, controls, tokens, f"capture-{i:02d}.log")
                if args.mode == "range":
                    item = parse_range(text)
                elif args.mode == "work":
                    item = parse_work(text)
                else:
                    work, census = parse_work(text), parse_range(text)
                    item = dict(work=work, census=census, joined=join_census(work, census))
                result.append(item)
                write_json(output / "captures.json", result)
                time.sleep(.3)
    finally:
        restore_live(d, h, output, {"DM_VK_TRACE_PASS": -1, "DM_VK_RANGE_TIMES": 0,
                                   "DM_VK_SHADER_WORK": 0}, env, settings, hardware=True)
    write_json(output / "result.json", dict(output=str(output), result=result, live_diagnostics_off=True,
               startup_timing_still_active=args.mode == "full", no_input_install_restart=True))
    print(output)


def ab(config, args):
    require(args.positioned, "Confirm the user-positioned scene")
    require(args.key in config.get("live_controls", []),
            "Declare the verified live key in session.live_controls before A/B")
    require(args.key not in {"DM_VK_DRIVER", "DM_VK_PASS_TIMES", "DM_VK_PASS_FULL", "DM_VK_PASS_SPIKE",
                            "DM_VK_PIPELINE_DIAG", "DM_VK_WORKLOAD_STATS", "DM_VK_UBWC"},
            "This startup control cannot be tested as a live switch")
    require(args.key in config.get("restore_controls", {}) and config["restore_controls"][args.key] == args.baseline,
            "Baseline must match an explicitly saved restore control; configure it after verifying live state")
    require(args.baseline != args.candidate, "Two distinct values required")
    d = Device(config)
    output = run_directory(config, "ab")
    h = History(d, output)
    cases = []
    env, settings = d.blob("env.txt"), d.blob("settings.txt")
    try:
        # Prewarm both paths; tracing and pipeline compilation must not be timed as improvements.
        for value in (args.candidate, args.baseline):
            d.control({args.key: value})
            time.sleep(args.warmup)
        schedule = [args.baseline if i % 2 == 0 else args.candidate for i in range(args.pairs*2+1)]
        for i, value in enumerate(schedule):
            d.verify()
            d.focused()
            d.control({args.key: value})
            anchor, began = h.sample(), time.monotonic()
            accepted, samples, replay = {}, [], []
            previous = None
            while time.monotonic()-began < args.seconds:
                tick = time.monotonic()
                sensors = d.sensors()
                row = h.sample()
                row["sensors"] = sensors
                accepted.update({f: v for f, v in row["completed"].items()
                                 if anchor["frame"]+args.exclude_frames <= f <= row["newest"]-12})
                if previous and row["frame"] > anchor["frame"]+args.exclude_frames:
                    df, dn = row["frame"]-previous["frame"], row["replay_ns"]-previous["replay_ns"]
                    if 5 < df < 200 and dn > 0:
                        replay.append(dn/df/1e6)
                samples.append(row)
                previous = row
                time.sleep(max(0, 1-(time.monotonic()-tick)))
            require(len(accepted) >= args.minimum_frames, "Too few stable frames in window; increase duration")
            cases.append(dict(index=i, value=value, gpu=stats(list(accepted.values())),
                              replay=stats(replay), frames=accepted, samples=samples))
            write_json(output / "cases.json", cases)
            print(i, value, cases[-1]["gpu"], flush=True)
    finally:
        restore_live(d, h, output, {args.key: args.baseline, "DM_VK_TRACE_PASS": -1,
                                   "DM_VK_RANGE_TIMES": 0, "DM_VK_SHADER_WORK": 0}, env, settings)
    paired = []
    for i in range(1, len(cases), 2):
        reference = (cases[i-1]["gpu"]["median"]+cases[i+1]["gpu"]["median"])/2
        paired.append(dict(index=i, reference_ms=reference, candidate_ms=cases[i]["gpu"]["median"],
                           percent=(cases[i]["gpu"]["median"]/reference-1)*100))
    write_json(output / "result.json", dict(cases=cases, paired=paired, restored=args.baseline,
               note="Completed submission spans include CPU-feed gaps. Review clocks/caps and all paired windows;pooled medians alone do not establish benefit. No automatic promotion."))
    print(output)


def timing_plan(config):
    d = Device(config)
    d.verify()
    output = run_directory(config, "timing-plan")
    normal = d.blob("env.txt")
    parsed = parse_env(normal)
    require(parsed.get("DM_VK_PASS_TIMES", "0") == "0", "Timer already enabled; complete the existing restoration first")
    lines = [x for x in normal.decode().splitlines() if not x.startswith(("DM_VK_PASS_TIMES=", "DM_VK_PASS_FULL=", "DM_VK_PASS_SPIKE="))]
    timing = ("\n".join(lines)+"\nDM_VK_PASS_TIMES=1\nDM_VK_PASS_FULL=1\nDM_VK_PASS_SPIKE=1\n").encode()
    (output / "normal-env.txt").write_bytes(normal)
    (output / "timing-env.txt").write_bytes(timing)
    write_json(output / "plan.json", dict(config=config["_path"], normal_sha256=sha(normal),
               timing_sha256=sha(timing), status="prepared-no-device-change", saves=config["save_roots"]))
    print(output / "plan.json")


def timing_restart(config, args):
    require(args.approved, "A human must authorize this particular shared-device restart")
    plan_path = Path(args.plan).resolve()
    checked_output(config["repo"], plan_path.parent)
    plan = __import__("json").loads(plan_path.read_text())
    require(Path(plan["config"]).resolve() == Path(config["_path"]), "Plan belongs to another session")
    d = Device(config)
    d.verify()
    before = "timing" if args.restore else "normal"
    after = "normal" if args.restore else "timing"
    require(plan["status"] == ("timing-active" if args.restore else "prepared-no-device-change"), "Wrong/reused startup plan")
    require(sha(d.blob("env.txt")) == plan[before+"_sha256"], "Environment changed; do not overwrite user edits")
    replacement = (plan_path.parent / (after+"-env.txt")).read_bytes()
    require(sha(replacement) == plan[after+"_sha256"], "Plan payload changed")
    # Verify run-as and save roots before stopping the shared app.
    d.call("shell", "run-as", d.package, "id")
    for root in plan["saves"]:
        require(re.fullmatch(r"files/[A-Za-z0-9_./-]+", root) and ".." not in root.split("/"), "Invalid save root")
        d.call("shell", "run-as", d.package, "ls", "-d", root)
    old_pid = d.pid()
    settings = d.blob("settings.txt")
    plan.update(status="restoring" if args.restore else "starting", previous_pid=old_pid,
                intended_environment=after, human_restart_authorized=True)
    write_json(plan_path, plan)
    d.shell("am force-stop " + d.package)
    archive = d.call("exec-out", "run-as", d.package, "tar", "-cf", "-", *plan["saves"])
    saves = archive_hashes(archive)
    backup = plan_path.parent / ("saves-before-" + after + ".tar")
    backup.write_bytes(archive)
    # No save is imported; the stopped latest committed files remain in place.
    remote = d.files + "/env.txt.gpu-profile-stage"
    d.call("push", str(plan_path.parent / (after+"-env.txt")), remote)
    require(d.remote_hash(remote) == sha(replacement), "Staged environment failed verification")
    d.shell("mv " + shlex.quote(remote) + " " + shlex.quote(d.files + "/env.txt"))
    d.shell("am start -n " + d.package + "/android.app.NativeActivity")
    for _ in range(40):
        time.sleep(.2)
        try:
            pid = d.pid()
        except ProfileError:
            continue
        if pid != old_pid:
            break
    else:
        raise ProfileError("Fresh process not observed; preserve plan and backup,inspect before further action")
    current = d.call("exec-out", "run-as", d.package, "tar", "-cf", "-", *plan["saves"])
    require(archive_hashes(current) == saves, "Committed saves changed across restart; do not restore an old save automatically")
    require(d.blob("settings.txt") == settings and d.blob("env.txt") == replacement, "Config changed across restart")
    config["assets"]["env.txt"] = sha(replacement)
    write_json(config["_path"], {k: v for k, v in config.items() if k != "_path"})
    d.verify()
    plan.update(status="normal-restored" if args.restore else "timing-active", pid=pid,
                latest_backup=str(backup), latest_save_hashes=saves, human_restart_authorized=True)
    write_json(plan_path, plan)
    print("Restart completed; latest saves verified; user must navigate manually. PID", pid)


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="command", required=True)
    init = sub.add_parser("init", help="Read-only identity/asset session setup")
    for key in ("adb", "serial", "package", "repo", "output", "elf", "nm"):
        init.add_argument("--" + key, required=True)
    init.add_argument("--host-name", default="libdmhost.so")
    for key in ("asset", "sensor", "save-root"):
        init.add_argument("--" + key, action="append")
    for name in ("inspect", "capture", "ab", "timing-plan", "timing-restart", "analyze-full"):
        q = sub.add_parser(name)
        q.add_argument("--config", required=True)
        if name == "capture":
            q.add_argument("--mode", choices=("baseline", "full", "range", "work", "joined", "state"), required=True)
            q.add_argument("--positioned", action="store_true")
            q.add_argument("--seconds", type=float, default=110)
            q.add_argument("--frames", type=int, default=3)
            q.add_argument("--first", type=int, default=0)
            q.add_argument("--last", type=int, default=0)
            q.add_argument("--range-mode", type=int, choices=range(1, 7), default=3)
            q.add_argument("--step", type=int, default=64)
            q.add_argument("--draw-first", type=int, default=0)
            q.add_argument("--draw-last", type=int, default=256)
            q.add_argument("--report-period", type=int, default=600)
            q.add_argument("--minimum-windows", type=int, default=3)
        if name == "ab":
            for key in ("key", "baseline", "candidate"):
                q.add_argument("--" + key, required=True)
            q.add_argument("--positioned", action="store_true")
            q.add_argument("--pairs", type=int, default=3)
            q.add_argument("--seconds", type=float, default=12)
            q.add_argument("--warmup", type=float, default=3)
            q.add_argument("--exclude-frames", type=int, default=30)
            q.add_argument("--minimum-frames", type=int, default=100)
        if name == "timing-restart":
            q.add_argument("--plan", required=True)
            q.add_argument("--approved", action="store_true")
            q.add_argument("--restore", action="store_true")
        if name == "analyze-full":
            q.add_argument("--directory", required=True)
    return p


def main():
    args = parser().parse_args()
    if args.command == "init":
        print(initialize(args)["output"])
        return
    config = load_config(args.config)
    if args.command == "inspect":
        d = Device(config)
        d.verify()
        host = Path(config["elf"]).read_bytes()
        print(dict(apk=config["apk_sha256"], pid=d.pid(), assets_match=True,
                   full_rows=b"GPU full pass row:" in host, range_rows=b"GPU range row:" in host,
                   workload_rows=b"shader workload row:" in host, presentation=b"present GPU steps (" in host,
                   sensors=d.sensors()))
    elif args.command == "capture":
        require(0 <= args.first <= args.last <= 511 and 0 < args.frames <= 20 and 0 < args.seconds <= 600,
                "Capture must be bounded;pass0..511,frames1..20,seconds0..600")
        require(1 <= args.step <= 4096 and 0 <= args.draw_first < args.draw_last <= 4096 and
                args.report_period > 0 and args.minimum_windows >= 1, "Invalid window/report limits")
        capture(config, args)
    elif args.command == "ab":
        require(1 <= args.pairs <= 8 and 2 <= args.seconds <= 60 and 0 <= args.warmup <= 20,
                "A/B must have1..8pairs,2..60s windows,0..20s warmup")
        ab(config, args)
    elif args.command == "timing-plan":
        timing_plan(config)
    elif args.command == "timing-restart":
        timing_restart(config, args)
    elif args.command == "analyze-full":
        output = checked_output(config["repo"], args.directory)
        import json
        manifest = json.loads((output / "manifest.json").read_text())
        result = analyze_full((output / "log.txt").read_text(), json.loads((output / "passes.json").read_text()),
                              manifest["anchor_frame"], manifest["end_frame"])
        write_json(output / "analysis.json", result)
        write_full_report(output, result)


if __name__ == "__main__":
    try:
        main()
    except (ProfileError, OSError, ValueError) as error:
        raise SystemExit(str(error))
