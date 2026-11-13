# Configuration and commands

All examples use placeholders. Resolve the project interpreter, ADB, serial,
package, matching unstripped ELF and LLVM tools for the current task. Scripts use
Python's standard library; no Conda, numpy, UI automation or remote service.

## Session

`gpu_profile.py init` reads the installed APK and verifies that its ARM64 host bytes
equal the supplied ELF. It hashes environment/settings plus repeatable `--asset`
paths, saves normal configuration locally, and creates `session.json` inside the
explicit `.gamedump/<App>/<run>` output. It does not write app files, restart or
install. The APK is processed in memory and is not included in the skill.

Important session fields:

| Field | Meaning |
|---|---|
| `adb`, `serial`, `package` | Explicit device adapter; no default serial/game |
| `repo`, `output` | Artifact containment under the chosen repository |
| `elf`, `elf_sha256`, `apk_sha256`, `llvm_nm` | Exact build identity and symbol tool |
| `host_name` | Native host mapping/entry name, normally `libdmhost.so` |
| `assets` | Relative remote file paths and baseline SHA-256 values |
| `sensors` | Actual readable `/sys/` paths for CPU policies, GPU clocks/busy and temperatures |
| `required_sensors` | Subset of those paths whose exact values must match, e.g. CPU caps |
| `restore_controls` | Explicit baseline values for A/B; initialized from normal env |
| `live_controls` | Explicit allow-list of controls verified live in this runtime;empty until configured |
| `save_roots` | Committed run-as save roots, normally `files/save`; add device save roots if used |

Do not configure arbitrary thermal-zone numbers from another device. Record each
zone's type and units; some sensors return stale values or deny access. An
unavailable clock is recorded as unavailable, not zero or a guessed lock.

`inspect --config <session.json>` verifies hashes and reports diagnostic string
capabilities and sensors without arming queries. If the APK changed, create a new
session using the new matching ELF; do not edit the old expected hash to bypass
identity checks. Keep old results as historical context.

## Capture commands

Every live capture needs `--positioned`, only after the human confirms the scene.
Choose a duration long enough for the game's actual FPS/report cadence.

```text
gpu_profile.py capture --config <session.json> --mode baseline --seconds 20 --positioned
gpu_profile.py capture --config <session.json> --mode full --seconds 110 --positioned
gpu_profile.py capture --config <session.json> --mode range --first <N> --last <M> --range-mode 3 --frames 3 --positioned
gpu_profile.py capture --config <session.json> --mode range --first <N> --last <M> --range-mode 4 --step 64 --frames 3 --positioned
gpu_profile.py capture --config <session.json> --mode range --first <N> --last <N> --range-mode 5 --draw-first <A> --draw-last <B> --frames 3 --positioned
gpu_profile.py capture --config <session.json> --mode work --first <N> --last <M> --frames 3 --positioned
gpu_profile.py capture --config <session.json> --mode joined --first <N> --last <M> --frames 2 --positioned
gpu_profile.py capture --config <session.json> --mode state --first <N> --last <M> --positioned
```

Range modes: 1 outside-pass phases; 2 adds internal begin/end markers; 3 envelope;
4 draw-step intervals plus census; 5 selected draw window plus census; 6 census
without timestamp commands. Windows are zero-based boundaries `(A,B]`; the first
actual draw has local ID1. Range timestamp capacity256 and census capacity1024 are
per submission. Query capacity4096 is also per submission. Narrow ranges when
overflow is reported; never accept dropped rows.

`joined` arms the no-timestamp census and invocation queries together. Both target
the next game frame, but a concurrent boundary can still separate them. The parser
rejects a mismatched frame/slot; retry one bounded frame instead of merging nearby
captures. No shader durations can be derived from counts alone.

State mode logs all actual draws of one selected pass, on separate frames for each
pass. It preserves raw descriptors/targets as evidence and exports write masks,
depth state, attributes and shader identity. These frames are unsuitable for
performance comparisons. Trace count/shape changes require a fresh scene map.

## Complete timers: coordinated startup cycle

```text
gpu_profile.py timing-plan --config <session.json>
gpu_profile.py timing-restart --config <session.json> --plan <plan.json> --approved
# User navigates to the chosen scene;then capture --mode full.
gpu_profile.py timing-restart --config <session.json> --plan <plan.json> --restore --approved
```

The first command only prepares local normal/timing environment payloads. Review
them before asking to restart. Coordinate both restart points with the user.
`--approved` is not a substitute for authorization in the conversation.

Each restart verifies identity/configuration, checks run-as/save paths, stops the
app, archives **current** committed saves, publishes a verified environment file,
starts the app and checks all committed file hashes again. No prior save is ever
imported. All save roots must be configured; a game's device save is not inferred.
APK installation is deliberately outside the CLI.

The source snapshot/profile/build toolchain and signing certificate remain those
of the currently authorized application. Never replace a user's test build with
an older diagnostic package to make a script work.

## Same-process A/B

```text
gpu_profile.py ab --config <session.json> --key <LIVE_KEY> --baseline <VALUE_A> --candidate <VALUE_B> --pairs 3 --seconds 12 --positioned
```

Baseline must match `restore_controls`; verify it before configuring the session.
Add only a key confirmed live by the runtime to `live_controls`. Startup keys cannot be A/B toggled.
Both paths are prewarmed; then A/B/A/B/A/B/A windows are captured with newest
history and transition frames excluded. Original value is restored in `finally`.
No automatic permanent promotion. Asset/settings hashes must remain unchanged.

Record actual CPU/GPU frequency samples as well as caps. `required_sensors` guards
abort changed caps; they cannot prove unchanged actual clocks. Interpret all paired
windows and tails, not only the pooled result. A granularity control can change
later passes as well as the chosen chain; re-map ordinals and qualify attribution.

## Output and recovery

Each operation creates a new directory. Raw logs, counter exports, maps, scene
trace, config snapshots and JSON accompany the report. Full mode produces
`all-gpu-steps.csv`, `analysis.json` and `REPORT.md`. Offline reanalysis:

```text
gpu_profile.py analyze-full --config <session.json> --directory <capture>
```

On error, read the last manifest/log first. Do not retry installation, overwrite
config, reuse stale pointers, or import an old save automatically. Live cleanup
turns range/work/state diagnostics off where the same PID survives. A cleanup
record is written to `cleanup.json`; if the app exits or is replaced,
controls are not sent to the new process and the original capture error is kept
with a cleanup note. An incomplete cleanup must not be reported as restored.
A startup-mode error may require coordinated restoration using the saved plan. If publishing or
starting fails halfway, inspect the actual env hash/process and the plan payloads
before another restart. Preserve the latest save archive and report the phase.
Finish with an explicit normal env/hash and diagnostics-off verification.
