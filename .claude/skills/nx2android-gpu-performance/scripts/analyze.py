"""Offline parsers. Keep command spans, invocations and presentation in separate units."""
from collections import Counter, defaultdict
import csv
import math
import re
import statistics

from profilelib import require, delta_ticks, stats


PASS = re.compile(r"--- pass (\d+): (\d+)x(\d+), colour formats (\d+) (\d+), depth format (\d+);(.*)")
FULL = re.compile(r"GPU full pass row: (render|dispatch)#(\d+) mean ([\d.]+) ms samples (\d+) size (\d+)x(\d+) depth_or_format (\d+) draws_or_z (\d+) coverage_width (\d+)")
RANGE_BEGIN = re.compile(r"GPU range begin: capture (\d+) frame (\d+) slot (\d+) mode (\d+) markers (\d+) dropped (\d+) bits (\d+) period ([\deE.+-]+) step (\d+) first_draw (\d+) last_draw (\d+) draw_rows (\d+)")
RANGE_ROW = re.compile(r"GPU range row: capture (\d+) frame (\d+) slot (\d+) query (\d+) pass (\d+) phase (\d+) part (\d+) (\d+)x(\d+) draws (\d+) stage (\d+) ticks (\d+) cpu_wall (\d+) cpu_thread (\d+)")
RANGE_NAMES = ("capture", "frame", "slot", "query", "pass", "phase", "part", "width", "height", "draws", "stage", "ticks", "cpu_wall", "cpu_thread")
DRAW = re.compile(r"GPU range draw: capture (\d+) frame (\d+) slot (\d+) ordinal (\d+) pass (\d+) local_draw (\d+) global_draw (\d+) vs ([\da-f]+) fs ([\da-f]+) depth (\d+)/(\d+)/(\d+)")
DRAW_NAMES = ("capture", "frame", "slot", "ordinal", "pass", "local_draw", "global_draw", "vs", "fs", "dt", "dw", "df")
WORK = re.compile(r"shader workload row: capture (\d+) frame (\d+) slot (\d+) query (\d+) pass (\d+) draw (\d+) (\d+)x(\d+) vs ([\da-f]+) fs ([\da-f]+) depth (\d+)/(\d+)/(\d+) IA (\d+)/(\d+) VS (\d+) CLIP (\d+)/(\d+) FS (\d+)")
WORK_NAMES = ("capture", "frame", "slot", "query", "pass", "draw", "width", "height", "vs", "fs", "dt", "dw", "df", "ia_vertices", "ia_primitives", "vs_invocations", "clip_invocations", "clip_primitives", "fs_invocations")


def records(pattern, names, text):
    return [{k: v.zfill(16) if k in ("vs", "fs") else int(v)
             for k, v in zip(names, m.groups(), strict=True)} for m in pattern.finditer(text)]


def parse_trace(text):
    passes = []
    for line in text.splitlines():
        if m := PASS.search(line):
            i, w, h, c0, c1, dep = map(int, m.groups()[:6])
            passes.append(dict(index=i, width=w, height=h, colour0=c0, colour1=c1, depth=dep,
                               attachments=m[7], shaders=[], barriers=[]))
        elif passes:
            if m := re.search(r"shaders at draw \d+: vs (\w+) fs (\w+)", line):
                pair = [v.zfill(16) for v in m.groups()]
                if pair not in passes[-1]["shaders"]:
                    passes[-1]["shaders"].append(pair)
            if m := re.search(r"ended from (0x[\da-f]+) \((\d+) draws\)", line):
                passes[-1].update(end_address=m[1], draws=int(m[2]))
            if m := re.search(r"barrier (0x[\da-f]+)", line):
                passes[-1]["barriers"].append(m[1])
    require(passes and [p["index"] for p in passes] == list(range(len(passes))),
            "Incomplete/noncontiguous scene trace; no fixed game-specific pass count is assumed")
    return passes


def analyze_full(text, passes, anchor, end, report_period=600, minimum_windows=3):
    by_pass = {p["index"]: p for p in passes}
    windows, pending, presentation, spikes = [], [], [], []
    frame = 0
    span_reports, replay_reports = {}, {}
    for line in text.splitlines():
        if m := re.search(r"replay up to present (\d+):", line):
            frame = int(m[1])
        if m := re.search(r"replay per frame \(last (\d+)\): .*?replay ([\d.]+) ms .*?GPU busy ([\d.]+) ms", line):
            if int(m[1]) == report_period:
                replay_reports[frame], span_reports[frame] = float(m[2]), float(m[3])
        if m := FULL.search(line):
            kind, i, ms, n, w, h, fmt, draws, coverage = m.groups()
            pending.append(dict(kind=kind, index=int(i), mean_ms=float(ms), samples=int(n),
                                width=int(w), height=int(h), format=int(fmt), draws_or_z=int(draws), coverage=int(coverage)))
        if "passes by GPU time (" in line:
            if anchor+report_period <= frame <= end:
                render = [x for x in pending if x["kind"] == "render"]
                require(len({(x["kind"], x["index"]) for x in pending}) == len(pending), "Duplicate full rows")
                require({x["index"] for x in render} == set(by_pass), "Pass sequence changed or rows lost")
                reference = statistics.median(x["samples"] for x in render)
                require(reference > 0 and abs(reference-report_period) <= max(1, report_period*.02), "Incomplete report window")
                for row in render:
                    ref = by_pass[row["index"]]
                    require((row["width"], row["height"], row["format"]) ==
                            (ref["width"], ref["height"], ref["colour0"]), "Pass dimensions/formats changed")
                windows.append(dict(frame=frame, reference_frames=reference, rows=pending))
            pending = []
        if m := re.search(r"present GPU steps \((\d+) game frames\): initial transitions/clear ([\d.]+) ms, final image blit ([\d.]+) ms, overlay preparation/render ([\d.]+) ms, rotation/final transition ([\d.]+) ms; total ([\d.]+) ms", line):
            if anchor+report_period <= frame <= end:
                n, *v = m.groups()
                presentation.append(dict(samples=int(n), clear_ms=float(v[0]), blit_ms=float(v[1]),
                                         overlay_ms=float(v[2]), rotation_ms=float(v[3]), total_ms=float(v[4])))
        if m := re.search(r"GPU spike: frame (\d+), ([\d.]+) ms in (\d+) passes and dispatches \(frame GPU ([\d.]+) ms, (-?[\d.]+) outside them\)", line):
            f, inside, n, total, residual = m.groups()
            if anchor+30 <= int(f) <= end-15 and float(total) > 0:
                spikes.append(dict(frame=int(f), inside_ms=float(inside), timed_items=int(n),
                                   submission_ms=float(total), residual_ms=float(residual)))
    require(len(windows) >= minimum_windows, "Too few complete positioned windows; extend capture")
    grouped = defaultdict(list)
    denominator = sum(w["reference_frames"] for w in windows)
    for window in windows:
        for row in window["rows"]:
            grouped[row["kind"], row["index"]].append((row, window["reference_frames"]))
    rows = []
    for (kind, i), values in sorted(grouped.items(), key=lambda x: (x[0][0] != "render", x[0][1])):
        count = sum(x["samples"] for x, _ in values)
        weighted = sum(x["mean_ms"]*x["samples"] for x, _ in values)
        row = values[-1][0]
        rows.append(dict(kind=kind, index=i, ms_per_frame=weighted/denominator, ms_per_call=weighted/count,
                         samples=count, width=row["width"], height=row["height"], format=row["format"],
                         shaders=by_pass[i]["shaders"] if kind == "render" else [],
                         window_ms_per_frame=[x["mean_ms"]*x["samples"]/n for x, n in values]))
    selected = [w["frame"] for w in windows]
    require(all(f in span_reports for f in selected), "Matching submission-span reports missing")
    total = statistics.mean(span_reports[f] for f in selected)
    render = sum(x["ms_per_frame"] for x in rows if x["kind"] == "render")
    compute = sum(x["ms_per_frame"] for x in rows if x["kind"] == "dispatch")
    pres = {key: sum(x[key]*x["samples"] for x in presentation)/sum(x["samples"] for x in presentation)
            for key in ("clear_ms", "blit_ms", "overlay_ms", "rotation_ms", "total_ms")} if presentation else None
    return dict(rows=rows, windows=windows, reference_frames=denominator, render_ms=render, compute_ms=compute,
                submission_ms=total, unassigned_ms=total-render-compute,
                replay_ms=statistics.mean(replay_reports[f] for f in selected), presentation=pres,
                presentation_windows=len(presentation), same_frame=spikes,
                note="Intrusive TOP/BOTTOM command/dependency spans,not pure hardware busy/ALU/texture/bandwidth. Compute ordinals may mix kernels;actual-call weighted. Presentation has separate windows. Residual is unassigned,not proved removable work.")


def parse_range(text):
    headers = list(RANGE_BEGIN.finditer(text))
    require(len(headers) == 1, "Expected one bounded range capture")
    v = headers[0].groups()
    cap, frame, slot, mode, n, dropped, bits = map(int, v[:7])
    period = float(v[7])
    step, lo, hi, ndraw = map(int, v[8:])
    require(1 <= mode <= 6 and dropped == 0 and 1 <= step <= 4096 and 0 <= lo < hi <= 4096,
            "Invalid configuration or overflow")
    delta_ticks(0, 0, bits, period)
    require(f"GPU range end: capture {cap} frame {frame} slot {slot}" in text, "Missing range completion")
    rows = records(RANGE_ROW, RANGE_NAMES, text)
    draws = records(DRAW, DRAW_NAMES, text)
    require(len(rows) == n and len(draws) == ndraw, "Incomplete markers/draw census")
    for i, row in enumerate(rows):
        require((row["capture"], row["frame"], row["slot"], row["query"]) == (cap, frame, slot, i), "Marker identity/order mismatch")
        require(row["stage"] == (1 if row["phase"] == 0 else 8192), "Wrong timestamp stage")
    for i, row in enumerate(draws):
        require((row["capture"], row["frame"], row["slot"], row["ordinal"]) == (cap, frame, slot, i), "Draw identity/order mismatch")
    if mode == 6:
        require(not rows, "Census mode must add no timestamp commands")
    result = []
    for p in sorted({x["pass"] for x in rows+draws}):
        rs = [x for x in rows if x["pass"] == p]
        ds = [x for x in draws if x["pass"] == p]
        if mode >= 4:
            require([x["local_draw"] for x in ds] == list(range(1, len(ds)+1)), "Actual draw census is not contiguous")
            require(all(b["global_draw"] > a["global_draw"] for a, b in zip(ds, ds[1:])), "Nonmonotonic global draws")
        else:
            require(not ds, "Unexpected census in control mode")
        if mode == 6:
            result.append(dict(pass_=p, draws=len(ds), intervals=[]))
            continue
        require(rs and len({x["part"] for x in rs}) == 1, "Missing/cross-part pass interval")
        require(len({(x["width"], x["height"]) for x in rs}) == 1, "Pass dimensions changed")
        count = rs[-1]["draws"]
        require(rs[0]["phase"] == 0 and rs[-1]["phase"] == 7, "Incomplete pass envelope")
        if mode in (1, 2, 3):
            phases = {1: [0, 1, 2, 3, 4, 7], 2: list(range(8)), 3: [0, 7]}[mode]
            require([x["phase"] for x in rs] == phases, "Missing/extra phases")
            require(all(x["draws"] == (count if x["phase"] >= 6 else 0) for x in rs), "Wrong draw boundary")
        elif mode == 4:
            expected = [(x, 0) for x in range(6)] + [(8, x) for x in range(step, count+1, step)] + [(6, count), (7, count)]
            require([(x["phase"], x["draws"]) for x in rs] == expected, "Wrong draw-step marker order")
        else:
            require(hi <= count and [(x["phase"], x["draws"]) for x in rs] ==
                    [(0, 0), (5, 0) if lo == 0 else (8, lo), (8, hi), (7, count)], "Wrong sparse window boundaries")
        if mode >= 4:
            require(len(ds) == count, "Prepared/actual draw counts differ")
        intervals = []
        for a, b in zip(rs, rs[1:]):
            require(b["cpu_wall"] >= a["cpu_wall"] and b["cpu_thread"] >= a["cpu_thread"], "CPU clocks reversed")
            subset = ds[a["draws"]:b["draws"]] if mode >= 4 else []
            intervals.append(dict(start_phase=a["phase"], end_phase=b["phase"], first=a["draws"], last=b["draws"],
                                  gpu_ms=delta_ticks(a["ticks"], b["ticks"], bits, period),
                                  cpu_record_ms=(b["cpu_wall"]-a["cpu_wall"])/1e6,
                                  pairs=[dict(vs=v, fs=f, draws=n) for (v, f), n in Counter((x["vs"], x["fs"]) for x in subset).items()]))
        result.append(dict(pass_=p, draws=count, width=rs[-1]["width"], height=rs[-1]["height"],
                           envelope_ms=delta_ticks(rs[0]["ticks"], rs[-1]["ticks"], bits, period), intervals=intervals))
    return dict(capture=cap, frame=frame, slot=slot, mode=mode, passes=result, draws=draws,
                note="Marker placement/density perturbs tiling. Command intervals are not isolated shader costs.")


def parse_work(text):
    headers = re.findall(r"shader workload begin: capture (\d+) frame (\d+) slot (\d+) queries (\d+) dropped (\d+)", text)
    require(len(headers) == 1, "Expected one workload capture")
    cap, frame, slot, n, dropped = map(int, headers[0])
    require(not dropped, "Workload query overflow")
    require(f"shader workload end: capture {cap} frame {frame} slot {slot}" in text, "Workload completion missing")
    rows = records(WORK, WORK_NAMES, text)
    require(len(rows) == n, "Workload rows missing or unsupported counter schema")
    for i, row in enumerate(rows):
        require((row["capture"], row["frame"], row["slot"], row["query"]) == (cap, frame, slot, i), "Workload identity/order mismatch")
    return dict(capture=cap, frame=frame, slot=slot, rows=rows,
                totals={k: sum(x[k] for x in rows) for k in WORK_NAMES[13:]},
                note="Invocations are not durations,utilization,visible pixels or theoretical speedup limits.")


def join_census(work, census):
    require(census["mode"] == 6 and (work["frame"], work["slot"]) == (census["frame"], census["slot"]),
            "Only join same-frame/slot census;capture serials are independent")
    by_draw = {(x["pass"], x["draw"]): x for x in work["rows"]}
    require(len(by_draw) == len(work["rows"]) == len(census["draws"]), "Duplicate/missing query draws")
    joined = []
    for draw in census["draws"]:
        row = by_draw.get((draw["pass"], draw["global_draw"]+1))
        require(row is not None and all(row[k] == draw[k] for k in ("vs", "fs", "dt", "dw", "df")), "Query/census actual-draw mismatch")
        joined.append(dict(draw, counters={k: row[k] for k in WORK_NAMES[13:]}))
    return joined


def parse_state(text):
    headers = re.findall(r"=== traced pass (\d+): (\d+)x(\d+), colour formats (\d+) (\d+), depth format (\d+)", text)
    require(len(headers) == 1, "Expected one pass-state trace")
    p, w, h, c0, c1, depth = map(int, headers[0])
    result, pending = [], None
    for line in text.splitlines():
        if "pipeline: topology " in line:
            require(pending is None, "Incomplete previous pipeline state")
            pending = dict(pipeline=line, masks={}, attributes=[], bindings=[])
        elif pending is not None:
            if m := re.search(r"blend (\d+): .*?write mask (0x[\da-f]+|\d+)", line):
                pending["masks"][int(m[1])] = int(m[2], 0)
            if "attribute location " in line:
                pending["attributes"].append(line)
            if re.search(r" binding \d+ stride ", line):
                pending["bindings"].append(line)
            if m := re.search(r"draw: .*?; shaders ([\da-f]+) ([\da-f]+) ([\da-f]+); targets (\d+)", line):
                require(len(pending["masks"]) == int(m[4]), "Missing attachment write masks")
                pending.update(vs=m[1], fs=m[2], gs=m[3], draw=line)
                result.append(pending)
                pending = None
    require(pending is None and result, "Incomplete/empty pipeline-state trace")
    return dict(pass_=p, width=w, height=h, colour0=c0, colour1=c1, depth=depth, pipelines=result,
                all_color_disabled=all(all(v == 0 for v in x["masks"].values()) for x in result),
                note="Logging frames are not timing baselines. Zero masks do not permit skipping draws/depth/queries or discarding existing image contents.")


def write_full_report(output, analysis):
    output = __import__("pathlib").Path(output)
    with (output / "all-gpu-steps.csv").open("w", encoding="utf-8-sig", newline="") as handle:
        fields = ["kind", "index", "ms_per_frame", "ms_per_call", "samples", "width", "height", "format"]
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in analysis["rows"]:
            writer.writerow({k: row[k] for k in fields})
    lines = ["# Complete GPU logical-step timing", "", analysis["note"], "",
             f"Matched game submission mean: {analysis['submission_ms']:.4f} ms. Render {analysis['render_ms']:.4f}, compute {analysis['compute_ms']:.4f}, unassigned {analysis['unassigned_ms']:.4f} ms.",
             "", "CPU replay overlaps GPU execution; do not add the two. Presentation is a separate submission/window. No effect labels are inferred from ordinal numbers.", "",
             "| Kind/ordinal | Size | ms/frame | Calls |", "|---|---|---:|---:|"]
    for row in analysis["rows"]:
        lines.append(f"| {row['kind']}#{row['index']} | {row['width']}x{row['height']} | {row['ms_per_frame']:.6f} | {row['samples']} |")
    lines += ["", "Presentation GPU stages (separate windows):", "", str(analysis["presentation"]), "",
              "Compute ordinals can contain different kernels when optional dispatches change. Inspect the same-scene trace for semantic labeling. Negative residuals are retained as an accounting discrepancy, never silently reclassified or clamped."]
    (output / "REPORT.md").write_text("\n".join(lines)+"\n", encoding="utf-8")
