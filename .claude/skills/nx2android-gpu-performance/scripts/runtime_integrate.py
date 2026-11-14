#!/usr/bin/env python3
"""Fail-closed diagnostic integration into a disposable nx2android source snapshot."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import re
import sys

RUNTIME = Path(__file__).resolve().parents[1] / "runtime"
PREFIX = Path("nxnative/dm/android")
MARK = "GPU_PROFILE_INTEGRATION_V1"


def one(text, old, new):
    if text.count(old) != 1:
        raise ValueError("Runtime source drift: expected one anchor: " + old[:100])
    return text.replace(old, new, 1)


def transform(files):
    """Pure transformation;validate every anchor before touching any source file."""
    original = dict(files)
    header, core, execute, present = [files[n] for n in ("gfx_vk.h", "gfx_vk_core.c", "gfx_vk_exec.c", "gfx_vk_present.c")]
    if MARK in header:
        for name in ("gfx_vk_range_diag.c", "gfx_vk_shader_work.c"):
            if files.get(name) != (RUNTIME / name).read_text():
                raise ValueError("Partial/modified previous integration: " + name)
        if not all(x in core for x in ("GPU full pass row:", "vk_rt_complete", "vk_sw_complete")) or \
                not all(x in execute for x in ("GPU_PROFILE_DRAW", "vk_rt_mark(", "vk_rt_draw(")) or \
                "present_gpu_begin" not in present:
            raise ValueError("Incomplete prior integration;manual audit required")
        return files
    if any(x in core for x in ("vk_rt_part(", "vk_sw_part(")):
        raise ValueError("Unmarked existing diagnostics;inspect rather than double-integrating")
    prototypes = """
// GPU_PROFILE_INTEGRATION_V1: bounded optional diagnostics,not rendering optimizations.
void vk_sw_part(VkCommandBuffer cb, int slot, int part, uint64_t frame);
uint32_t vk_sw_begin(VkCommandBuffer cb, uint64_t vs, uint64_t fs, uint32_t pass, uint32_t draw,
                     uint32_t w, uint32_t h, uint32_t dt, uint32_t dw, uint32_t df);
void vk_sw_end(VkCommandBuffer cb, uint32_t query);
void vk_sw_complete(uint64_t frame, int slot);
void vk_rt_part(VkCommandBuffer cb, int slot, int part, uint64_t frame);
void vk_rt_mark(VkCommandBuffer cb, uint32_t pass, uint32_t phase, uint32_t w, uint32_t h, uint32_t draws);
void vk_rt_complete(uint64_t frame, int slot);
void vk_rt_draw(VkCommandBuffer cb, uint32_t pass, uint32_t draw, uint32_t global, uint32_t w, uint32_t h,
                uint64_t vs, uint64_t fs, uint32_t dt, uint32_t dw, uint32_t df);
"""
    header = one(header, "#include <vulkan/vulkan.h>", "#include <vulkan/vulkan.h>\n" + prototypes)
    header = one(header, "int has_bc, has_astc,", "int has_shader_work; uint32_t range_timestamp_bits;\n    int has_bc, has_astc,")
    for function in ("vkCmdBeginQuery", "vkCmdEndQuery"):
        if "X(" + function + ")" not in header:
            header = one(header, "X(vkCreateQueryPool)", "X(" + function + ") X(vkCreateQueryPool)")
    core = one(core, '    if (V.qf == qn) die("[vk] no graphics + compute queue");',
               '    if (V.qf == qn) die("[vk] no graphics + compute queue");\n    V.range_timestamp_bits = qp[V.qf].timestampValidBits;')
    core = one(core, "    vkGetPhysicalDeviceFeatures2(V.pd, &ff);", "    vkGetPhysicalDeviceFeatures2(V.pd, &ff);\n    V.has_shader_work = ff.features.pipelineStatisticsQuery;")
    core = one(core, "#define NSLOT 6", "#define NSLOT 6\nstatic uint64_t g_cur_frame;")
    core = one(core, "static uint64_t g_cur_frame, g_frame_gpu[64], g_frame_gpu_of[64];", "static uint64_t g_frame_gpu[64], g_frame_gpu_of[64];")
    start, end = core.index("// DM_VK_PASS_TIMES=1:"), core.index("// one submission at a time:")
    core = core[:start] + (RUNTIME / "pass_times_block.c.inc").read_text() + core[end:]
    # Older sources directly modified the shared accumulator;newer ones already serialize it.
    core, count = re.subn(r"int f = \(int\)c->pmeta\[i\]\.fidx; g_pacc\[f\] \+= \(double\)\(t\[2 \* i \+ 1\] - t\[2 \* i\]\) \* V\.props\.limits\.timestampPeriod / 1e6; g_pcnt\[f\]\+\+; g_plast\[f\] = c->pmeta\[i\]; if \(f \+ 1 > g_pmax\) g_pmax = f \+ 1;",
        "pass_accumulate((double)(t[2 * i + 1] - t[2 * i]) * V.props.limits.timestampPeriod / 1e6, &c->pmeta[i]);", core)
    completion = core[core.index("static void *completion_main"):]
    if count > 1 or "pass_accumulate(" not in completion:
        raise ValueError("Unknown completion-accumulator layout")
    core = one(core, "VKCHK(vkBeginCommandBuffer(s->main, &bi)); g_cmd_gen++;\n}",
               "VKCHK(vkBeginCommandBuffer(s->main, &bi)); g_cmd_gen++;\n    vk_sw_part(s->main, g_cur, s->nmain_used - 1, __atomic_load_n(&g_cur_frame, __ATOMIC_RELAXED));\n    vk_rt_part(s->main, g_cur, s->nmain_used - 1, __atomic_load_n(&g_cur_frame, __ATOMIC_RELAXED));\n}")
    core = one(core, "    VKCHK(vkBeginCommandBuffer(s->main, &bi)); g_cmd_gen++;\n    // copied pools:",
               "    VKCHK(vkBeginCommandBuffer(s->main, &bi)); g_cmd_gen++;\n    vk_sw_part(s->main, g_cur, 0, __atomic_load_n(&g_cur_frame, __ATOMIC_RELAXED));\n    vk_rt_part(s->main, g_cur, 0, __atomic_load_n(&g_cur_frame, __ATOMIC_RELAXED));\n    // copied pools:")
    anchor = "        if (g_shadow_pools && c->rslot >= 0) pools_copy_back(&g_slot[c->rslot]);"
    core = one(core, anchor, anchor + "\n        if (c->slot >= 0) { vk_sw_complete(c->frame, c->slot); vk_rt_complete(c->frame, c->slot); }")
    execute = one(execute, "    if (!g_x_nobar) vk_img_barrier_all(cb);\n    if (cut_touch) lrz_touch(dim, (g_lrz_touch & LRZ_CUT) != 0);",
                  "    vk_rt_mark(cb, (uint32_t)g_sub_pass, 0, w, h, 0);\n    if (!g_x_nobar) vk_img_barrier_all(cb);\n    vk_rt_mark(cb, (uint32_t)g_sub_pass, 1, w, h, 0);\n    if (cut_touch) lrz_touch(dim, (g_lrz_touch & LRZ_CUT) != 0);\n    vk_rt_mark(cb, (uint32_t)g_sub_pass, 2, w, h, 0);")
    execute = one(execute, "    vkimg_t *dim = S.daspect ? P_att[8].im : NULL;", "    vk_rt_mark(vk_cmd(), (uint32_t)g_sub_pass, 3, S.pass_w, S.pass_h, 0);\n    vkimg_t *dim = S.daspect ? P_att[8].im : NULL;")
    anchor = "    if ((g_lrz_touch & LRZ_READ) && dim && P_da.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) lrz_touch(dim, 0);"
    execute = one(execute, anchor, anchor + "\n    vk_rt_mark(vk_cmd(), (uint32_t)g_sub_pass, 4, S.pass_w, S.pass_h, 0);")
    anchor = "    S.pass_open = 1; g_vkst.passes++; g_sub_pass++; g_pass_wmem = 0;"
    execute = one(execute, anchor, anchor + "\n    vk_rt_mark(vk_cmd(), (uint32_t)(g_sub_pass - 1), 5, S.pass_w, S.pass_h, 0);")
    anchor = "if (S.rp_pass) vkCmdEndRenderPass(vk_cmd()); else vkCmdEndRendering(vk_cmd()); void vk_pass_ts"
    execute = one(execute, anchor, "vk_rt_mark(vk_cmd(), (uint32_t)(g_sub_pass - 1), 6, S.pass_w, S.pass_h, (uint32_t)g_pass_draws); "
                  "if (S.rp_pass) vkCmdEndRenderPass(vk_cmd()); else vkCmdEndRendering(vk_cmd()); "
                  "vk_rt_mark(vk_cmd(), (uint32_t)(g_sub_pass - 1), 7, S.pass_w, S.pass_h, (uint32_t)g_pass_draws); void vk_pass_ts")
    wrapper = """// Queries wrap only actual commands;preparation,draw arguments and pass boundaries are unchanged.
#define GPU_PROFILE_DRAW(cb_, command_) do { \\
    uint32_t q_ = vk_sw_begin((cb_), S.sh[0] ? S.sh[0]->key : 0, S.sh[1] ? S.sh[1]->key : 0, \\
        (uint32_t)(g_sub_pass - 1), (uint32_t)g_draw_i, S.pass_w, S.pass_h, S.ds.dt, S.ds.dw, S.ds.df); \\
    command_; if (q_) vk_sw_end((cb_), q_); \\
    vk_rt_draw((cb_), (uint32_t)(g_sub_pass - 1), (uint32_t)g_pass_draws, (uint32_t)(g_draw_i - 1), S.pass_w, S.pass_h, \\
        S.sh[0] ? S.sh[0]->key : 0, S.sh[1] ? S.sh[1]->key : 0, S.ds.dt, S.ds.dw, S.ds.df); \\
} while (0)
"""
    for api in ("vkCmdDraw", "vkCmdDrawIndexed", "vkCmdDrawIndirect", "vkCmdDrawIndexedIndirect",
                "vkCmdDrawIndirectCount", "vkCmdDrawIndexedIndirectCount", "vkCmdDrawIndirectByteCountEXT"):
        wrapper += f"#define {api}(cb_, ...) GPU_PROFILE_DRAW(cb_, {api}(cb_, __VA_ARGS__))\n"
    execute = one(execute, "static void draw(const p_draw *d);", wrapper + "\nstatic void draw(const p_draw *d);")
    present = one(present, "static void present_window(", (RUNTIME / "present_gpu_times.c.inc").read_text()+"\nstatic void present_window(")
    hooks = [
        ("    vkWaitForFences(V.dev, 1, &g_pfence[k], 1, UINT64_MAX);", "    VkResult waited = vkWaitForFences(V.dev, 1, &g_pfence[k], 1, UINT64_MAX);\n    if (waited != VK_SUCCESS) { VKLOG(\"[RED] present fence wait failed: %d\", waited); return; }\n    present_gpu_recycle(k);"),
        ("    vk_img_barrier_all(cb);   // after everything submitted before on this queue", "    vk_img_barrier_all(cb);   // after everything submitted before on this queue\n    present_gpu_begin(cb, k, ticket);"),
        ("    vkCmdClearColorImage(cb, tgt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rg);", "    vkCmdClearColorImage(cb, tgt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rg);\n    present_gpu_mark(cb, k, 1);"),
        ("    vkCmdBlitImage(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, tgt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);", "    vkCmdBlitImage(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, tgt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);\n    present_gpu_mark(cb, k, 2);"),
        ("    if (turned) {\n        barrier(cb, g_rot.img, lay, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);", "    present_gpu_mark(cb, k, 3);\n    if (turned) {\n        barrier(cb, g_rot.img, lay, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);"),
        ("    } else barrier(cb, g_simg[idx], lay, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);\n    vkEndCommandBuffer(cb);", "    } else barrier(cb, g_simg[idx], lay, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);\n    present_gpu_mark(cb, k, 4);\n    vkEndCommandBuffer(cb);"),
        ('    if (vkQueueSubmit(V.q, 1, &si, g_pfence[k])) { pthread_mutex_unlock(&g_vk_queue_mu); VKRED_ONCE(91, "present submit failed"); return; }', '    if (vkQueueSubmit(V.q, 1, &si, g_pfence[k])) { present_gpu_submitted(k, 0); pthread_mutex_unlock(&g_vk_queue_mu); VKRED_ONCE(91, "present submit failed"); return; }\n    present_gpu_submitted(k, 1);')]
    for old, new in hooks:
        present = one(present, old, new)
    files = dict(original, **{"gfx_vk.h": header, "gfx_vk_core.c": core,
                             "gfx_vk_exec.c": execute, "gfx_vk_present.c": present})
    for name in ("gfx_vk_range_diag.c", "gfx_vk_shader_work.c"):
        if name in files:
            raise ValueError("Existing unmarked module: " + name)
        files[name] = (RUNTIME / name).read_text()
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, help="Disposable source snapshot,not an APK/game directory")
    parser.add_argument("--apply", action="store_true", help="Default only writes a reviewable diff to stdout")
    args = parser.parse_args()
    root = Path(args.repo).resolve()
    if args.apply and ".gamedump" not in root.parts:
        raise ValueError("Apply only to a disposable source snapshot under .gamedump/<App>/")
    originals = {name: (root / PREFIX / name).read_text(encoding="utf-8")
                 for name in ("gfx_vk.h", "gfx_vk_core.c", "gfx_vk_exec.c", "gfx_vk_present.c")}
    for name in ("gfx_vk_range_diag.c", "gfx_vk_shader_work.c"):
        if (root / PREFIX / name).exists():
            originals[name] = (root / PREFIX / name).read_text()
    changed = transform(originals)
    for name, text in changed.items():
        sys.stdout.writelines(difflib.unified_diff(originals.get(name, "").splitlines(True), text.splitlines(True),
                                                fromfile="a/"+str(PREFIX/name), tofile="b/"+str(PREFIX/name)))
    if not args.apply:
        return
    if changed == originals:
        return
    backup = root / ".gpu-profile-source-backup"
    if backup.exists():
        raise ValueError("Backup already exists;review/restore instead of overwriting it")
    backup.mkdir()
    manifest = {}
    for name, text in changed.items():
        if text == originals.get(name):
            continue
        path = root / PREFIX / name
        manifest[name] = dict(existed=name in originals, sha256=hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None)
        if path.exists():
            (backup / name).write_bytes(path.read_bytes())
    (backup / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    try:
        for name, text in changed.items():
            if text != originals.get(name):
                (root / PREFIX / name).write_text(text, encoding="utf-8")
    except OSError:
        for name, info in manifest.items():
            path = root / PREFIX / name
            if info["existed"]:
                path.write_bytes((backup / name).read_bytes())
            elif path.exists():
                path.unlink()
        raise


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError) as error:
        raise SystemExit(str(error))
