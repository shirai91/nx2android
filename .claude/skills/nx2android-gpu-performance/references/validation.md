# Validation

The package's tests use synthetic counters/logs and source text,not game data.
Run them with the repository interpreter:

```text
<python> -m unittest discover <skill>/tests -v
<python> <skill>/scripts/gpu_profile.py --help
<python> <skill>/scripts/runtime_integrate.py --help
```

They cover APK/ELF and mapped-history boundaries,slot/tag validation,save-archive
paths,full-window exclusion and rare-compute weighting,range phases/draw identity,
same-frame counter/census joins,state masks,source integration/idempotence/drift,
and injected collector failures/restoration. Native harnesses exercise packaged
query modules without a driver when a suitable C compiler is available;an NDK
cross-compile can validate buildability but not device execution.

Before a source commit,run the full repository test suite too. Preserve unrelated
dirty work. Stage only this skill directory for a skill-only commit and inspect
the exact staged path list/diff. Verify no game inputs,APKs,SPIR-V,keys,saves,
hard-coded developer paths,device serials or game package/ordinal assumptions
entered the skill. Report actual skips/failures rather than claiming unavailable
platform checks passed.

Device verification is separate:read-only identity check on the current APK;
bounded positioned captures;all temporary controls restored. Reusable extraction
does not authorize installing a diagnostic build or repeating game inputs. Full
timers must be disabled through the coordinated restart cycle before normal play.

## Extraction validation (2026-11-13)

- Skill suite: 22 tests, zero failures/errors, one skip because a POSIX host C
  compiler was unavailable. The query lifecycle harness and standalone pipeline
  compiler probe were separately cross-compiled with the Android NDK. They were
  not executed on a device during extraction.
- Historical evidence replay: 84 capture files parsed successfully. Complete
  accounting matched the prior analyzer, including 61 render and five compute
  rows. Game-derived fixtures remain in ignored local storage, outside the skill.
- Source integration: applied to a clean repository source snapshot; instrumented
  host builds passed for three different game profiles. Source drift and partial
  integration are rejected by the offline tests.
- Repository suite: 188 tests, zero failures/errors, 66 skips. The local run used
  the installed .NET SDK and mapped Windows `python3` subprocess requests to the
  project interpreter. Three Java-dependent tests were explicitly skipped because
  `javac` was unavailable; other skips followed platform/toolchain requirements.
  These execution adapters changed no repository source.
- Device state: no install, restart, game input or new live capture was performed
  while extracting and validating this package. Historical captures validate
  compatibility with their recorded builds, not every future runtime or driver.
