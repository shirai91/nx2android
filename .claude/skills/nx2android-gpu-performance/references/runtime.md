# Runtime capability and integration

The collector supports the existing nx2android debug host's DM_CTL protocol and
64-slot frame history. Release APKs ignore DM_CTL and generally lack required
symbols. Use the matching debug ELF,not a similarly named or rebuilt library.
The reader validates symbol sizes:history/time tags512bytes each,frame8bytes and
replay counters104bytes. A changed ABI requires an adapter and tests,not guessed
offsets. Revalidate APK/ELF after every reinstall;re-resolve maps after every PID.

## Capability layers

1. **Ordinary counters:**existing log/replay/history,without new queries.
2. **Complete timers:**`DM_VK_PASS_TIMES=1`, `DM_VK_PASS_FULL=1` at startup.
   All render/compute rows and separate presentation stages.
3. **One-frame command ranges:**`DM_VK_RANGE_TIMES=1..6` and explicit pass/window
   controls. Modes snapshot at arm time;completion validates recycled slots.
4. **One-frame invocation queries:**`DM_VK_SHADER_WORK=1` and explicit first/last
   passes. Requires Vulkan `pipelineStatisticsQuery`. Six64-bit counters per draw.
5. **Existing state trace:**`DM_VK_TRACE_PASS=<N>` once,or `-2` for a scene map.
6. **Optional compiler inspection:**the executable-properties extension,with a
   standalone probe or existing runtime pipeline diagnostics.

Default-off range/work modules create no query pool until armed. Diagnostics still
have host call/check overhead in an instrumented build. Query captures change
execution;do not compare their frame time to an ordinary performance baseline.
Completion hooks must run only after successful submitted work's fence. Dropped
work must not expose stale queries. Presentation slots are read only after a
successful wait;failed submit paths are marked invalid.

## Included source assets

`runtime/` contains engine code only:bounded range and invocation modules,full pass
accumulation/reporting block,presentation marker block and a compiler probe. No
game binary,shader,driver,APK,key,save or machine path is included. `assets.json`
records exact source hashes. These are diagnostic assets,not runtime optimizations.

`runtime_integrate.py` targets the known nx2android source shapes and fails closed
if anchors drift. It does **not** replace an entire graphics backend or copy a
profile. It validates all changes in memory before source mutation:

```text
<python> <skill>/scripts/runtime_integrate.py --repo <disposable-source> > <run>/instrumentation.patch
# Review the patch;all game/profile/settings/driver files must be absent.
<python> <skill>/scripts/runtime_integrate.py --repo <disposable-source> --apply
```

Use a disposable source snapshot under `.gamedump/<App>/`. The dry run writes no
source. `--apply` creates `.gpu-profile-source-backup/` before updating six files
and rolls back write failures. A later intentional source change needs a fresh
snapshot;do not overwrite its backup. An unmarked existing diagnostic integration
is rejected to avoid double wrapping commands. An exactly marked integration is
recognized;verify all packaged modules and hooks when updating it.

The full pass block preserves independent render/dispatch ordinals and serialized
accumulation. Actual draw wrappers preserve arguments and preparation. Markers do
not introduce rendering/command-buffer cuts or remove barriers. Existing LRZ,
alpha/depth,aliasing and synchronization repairs remain in the source snapshot.
The skill never selects game-specific heap/resolution modifications.

## Build and device validation

Build using the current authorized profile/toolchain and its existing custom
driver path. The backend build automatically includes its `.c` files. Example:

```text
<python> <snapshot>/nxnative/dm/android/build.py --game <title-id> --ndk <NDK> --out <run>/host-debug --lib
```

Do not add `--release` for DM_CTL captures. Preserve actual profile-generated
macros,expanded memory budgets and driver/APK entries rather than rebuilding from
a generic profile that silently changes them. Package/sign using the existing
project pipeline. Name the concrete package and size before requesting installation.
Use a separate test package when appropriate. Preserve the newest saves and check
hashes across any approved install/restart. This skill contains no installer.

Run offline parser/session tests and runtime harnesses,then relevant native/runtime
checks and the repository regression suite. If the local platform cannot execute
native tests,report skips honestly;an NDK compile is not a rendered device check.
After installation verify actual query support and render sizes in the app before
asking the user to repeat long reproduction procedures.
