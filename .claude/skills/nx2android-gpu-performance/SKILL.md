---
name: nx2android-gpu-performance
description: Measure and investigate CPU replay and Vulkan GPU performance in any nx2android Android game. Use for complete render/compute/presentation timings, shader and draw hotspots, actual invocation counts, attachment/dependency analysis, resolution/clock comparisons, and same-scene A/B validation. Includes configurable device collectors, offline analyzers, matching-APK/ELF guards, optional runtime instrumentation and compiler inspection. Does not change game quality or apply optimizations automatically.
---

# nx2android GPU performance investigation

Use this skill to move from a whole-frame budget to a measured optimization target.
It supports different games, packages, Android devices, drivers, resolutions and
scene/pass layouts. It is an nx2android Vulkan adapter, **not** a profiler for
arbitrary Android applications or a hardware utilization counter.

Read the project's `AGENTS.md`, `docs/ARCHITECTURE.md`, `docs/DEVELOPMENT_GUIDE.md`
and existing device workflow first. Apply the user's current permissions and
constraints. Use the project Python interpreter. Paths in commands below are
placeholders supplied by the session; nothing assumes one PC, device or title.

## Non-negotiable measurement rules

1. Preserve the current installation, latest saves, driver, quality, existing fixes
   and retained optimizations. Obtain concrete authorization before installation.
   Coordinate restarts while the device is shared. An `--approved` CLI flag records
   human authorization; it does not create it. Navigation remains the user's unless
   they explicitly authorize game inputs. These scripts never send game inputs.
2. Start with a verified APK and matching unstripped host ELF. Reinitialize after
   any reinstall. Never reuse another build's addresses or treat cross-build
   changes as an optimization gain. Guards reject mismatched assets, PID and ABI.
3. Store every game-derived capture/build under `.gamedump/<App>/<run>/`, not in
   this skill, a source commit, a temp game folder or a user's download directory.
   Keep the baseline and raw evidence. Do not commit APKs, SPIR-V, keys or saves.
4. Compare the same scene/view/settings in one process where possible. Record
   actual clocks and caps, temperatures and frame policy. A power-saving mode or
   an overlay reading is an observed condition, not a physical frequency lock.
   Equal caps do not imply equal actual CPU/GPU clocks.
5. Separate CPU replay, completed game-submission timestamp spans, render/compute
   intervals, presentation work and physical display timing. They overlap or have
   different sample windows. Never add them into an invented elapsed frame time.
6. Invocation counts, static instructions, compiler wave limits and stall estimates
   are different evidence. None is a duration share, measured occupancy, visible
   pixel count or speedup bound. Binning variants can differ from main programs.
7. Timestamps/queries/logging perturb execution. Establish an ordinary baseline,
   measure complete logical steps, then narrow hotspots. Internal markers can move
   tiled work into `render_begin`; do not rename that interval "attachment load
   time" or "CPU vkBegin cost" without a controlled test.
8. Keep every test bounded. Restore live controls in `finally`; startup timers
   require a coordinated restart to disable. Verify restoration. Do not promote,
   commit or publish a candidate without the user's request and required checks.

## Included tools

| Component | Purpose |
|---|---|
| `scripts/gpu_profile.py` | Identity setup/inspection, baseline/full captures, bounded ranges, invocation/census joins, pipeline states, A/B windows, startup timing plans and save-preserving restart |
| `scripts/profilelib.py` | Parameterized ADB adapter, APK/ELF/assets/PID validation, mapped 64-slot GPU-history reading, sysfs sampling, archive validation |
| `scripts/analyze.py` | Device-independent strict parsers, complete-window weighting, range validation, same-frame joins, CSV/Markdown reports |
| `scripts/runtime_integrate.py` | Reviewable, fail-closed integration into a disposable runtime source snapshot; default is dry run |
| `runtime/` | Default-off per-draw counters, phase/draw timestamps, complete pass and presentation instrumentation, optional compiler probe |
| `tests/` | Offline regression fixtures and native diagnostic-module harness generation; no game inputs or device install |

No script reads another investigation's `*-current.txt` files. Session JSON holds
all paths, serial/package, asset hashes, sensors and restore values. Core analyzers
accept any complete pass count and dimensions, not a named game's ordinals.

## Workflow

### 1. Preserve progress and confirm scope

Before switching tools or tasks, write a local progress checkpoint with current
APK/ELF/asset identities; last verified device state; scene and settings;
retained/rejected candidates; unresolved hypotheses; exact artifact paths; and the
next action. Use [the checkpoint template](references/checkpoint-template.md).
Never reset the user's latest saves to reproduce an old checkpoint.

### 2. Create and inspect a session

Read [setup and commands](references/commands.md). Initialize explicitly:

```powershell
<python> <skill>/scripts/gpu_profile.py init --adb <adb> --serial <serial> --package <test.package> --repo <repo> --output <repo>/.gamedump/<App>/<run> --elf <matching-libdmhost.so> --nm <llvm-nm> --asset shaders.dmsv --asset mods/main.patched.bin
<python> <skill>/scripts/gpu_profile.py inspect --config <run>/session.json
```

Configure the actual hardware's sysfs paths/caps in `sensors`/`required_sensors`.
Save the scene/view, expected quality and manual clock observation in a session
note. Read-only identity setup does not establish that the user is positioned.

If instrumentation is absent, use [runtime integration](references/runtime.md).
Prepare/build/review a concrete debug package first; ask before installing it.
Never silently downgrade/overwrite a test build another task installed.

### 3. Establish the ordinary whole-frame baseline

After the user confirms positioning, collect existing history with `capture
--mode baseline --positioned`. Record CPU replay, tail behavior, pipeline builds,
uploads, hardware state and presentation policy. Do not wait for thermal equilibrium
when the user explicitly wants immediate work; label warming conditions and use
short reversals. Keep claims proportional to the actual test duration.

### 4. Account for the complete GPU work

If the user wants all steps, use **full mode before choosing tiny shader windows**.
Complete rows require `DM_VK_PASS_TIMES=1` and `DM_VK_PASS_FULL=1` at startup.
`timing-plan` only creates local payloads. Coordinate `timing-restart --approved`,
verify latest save hashes, let the user navigate, then capture full mode.

Capture enough post-position frames for at least three complete report windows.
At low FPS, increase duration within the bounded limits; do not lower the minimum
to include startup/loading data. Output all render rows, occurrence-weighted
compute rows, same-window submission totals and unassigned residual. Present the
separate presentation stages and all rows in `REPORT.md`/CSV. Group roles from
actual shader/resource/attachment evidence, never from a fixed ordinal map.

Follow [metrics and interpretation](references/metrics.md). Residual outside
passes is not automatically memory bandwidth, barriers or removable GPU work.
Presentation batches have their own window boundaries. Optional compute dispatches
can shift compute ordinals; label mixed kernels rather than attributing one name.

Coordinate the restore restart after capture. `timing-restart --restore --approved`
uses the plan's original environment and backs up current saves again; it never
imports a previous save. Do not leave diagnostics active without telling the user.

### 5. Localize the largest logical chain

Use one-frame range envelopes, then coarse/fine phase markers, draw steps and sparse
windows. Always check complete-frame and full-chain relevance. Compare marker
densities and a sparse control. Preserve pass/shader/depth identities and actual
draw counts. Large `render_begin` spans on tiled GPUs are an attribution warning.

Use `capture --mode work` for actual IA/VS/clipping/FS calls. Use `--mode joined`
only when exact same-frame query/census association is needed; the parser rejects
different frames/slots/programs/depth state and never aligns by "nearby" frames.
Counter serial numbers are independent and need not equal range capture serials.

Use state mode for write masks, depth tests/writes, forced fragment-depth variants,
vertex formats and bindings. A bound sampled image is not proof a shader executed
that sample. Pool-relative offset equality alone does not establish image identity.
Zero color masks do not justify skipping depth/alpha/query/side-effect work.

### 6. Test one hypothesis and retain only verified gains

Use alternating baseline/candidate windows in the same process after both paths
are warm. Match clocks/caps as far as the device allows; report remaining variation.
Inspect all reversals and tails. Never rely solely on pooled medians or compare
absolute timing across different scenes. Granularity changes can renumber passes;
validate role/attachment mapping again. Keep compilation/query/logging frames out
of ordinary timing. The A/B CLI restores the explicit baseline even on failure.

For shader candidates, prove semantic/address/bit equivalence as appropriate,
validate SPIR-V/reflection and compile with the actual driver before device A/B.
Use [compiler inspection](references/compiler.md); reconstructed interfaces are
not exact live pipeline captures. Reducing instructions is evidence of better
code, not proof of critical-path or whole-frame improvement.

After repeatable gains, coordinate a dynamic visual check: movement, camera,
scene changes and restart persistence as relevant. Keep candidates reversible.
Do not revive a correctness-failing draw-skip/precision shortcut merely because
it reduced work in a static screenshot.

### 7. Finish with a reviewable result

Report what dominates the **whole** frame, observations versus hypotheses, what
changed, accepted frames/windows, all reversals, clock/thermal confounds,
limitations and exact restored state. Save raw evidence and a resume checkpoint.
Say which ignored artifact directory remains and why. A profiler extraction
commit must contain only the skill and its tests/docs/assets, not unrelated core
changes or game data. Run relevant checks and the repository suite before commit.

## References

- [Commands, configuration and recovery](references/commands.md)
- [Metric meanings and known traps](references/metrics.md)
- [Runtime capabilities, source integration and build checks](references/runtime.md)
- [Compiler statistics and state spec](references/compiler.md)
- [Checkpoint and result template](references/checkpoint-template.md)
- [Validation commands and coverage](references/validation.md)
