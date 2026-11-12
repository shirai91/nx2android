# Development guide

How to run nx2android from the repository, build the player's bundle and send changes. Players need none of this: the [README](../README.md) and [packaging/INSTALL.md](../packaging/INSTALL.md) cover the app. How the pieces fit together: [ARCHITECTURE.md](ARCHITECTURE.md).

## Setup from the repository

Install [Python](https://www.python.org) 3.11 or later and `python -m pip install -r requirements.txt`, then `python -m porter tools --download` (or install them yourself):
- [Android NDK r27c](https://developer.android.com/ndk/downloads) (only the NDK: no Android Studio, SDK or JDK)
- [.NET SDK 10](https://dotnet.microsoft.com/download)
- Android [platform-tools](https://developer.android.com/tools/releases/platform-tools) (adb), to install from the tool
- your own `prod.keys`, dumped from your Switch

## Port a game

The app:

```
python -m gui
```

Check the **Setup** tab, choose the game's folder (the base game NSP, XCI, NSZ or XCZ, its update and DLC) on the **Port** tab, and click **Port**.

Keep the base game, update and DLC in separate files. When a dump's tickets or readable
content-meta headers identify several titles in one file, porter warns which title it selects
and which embedded titles it will not select as updates or DLC. It does not split merged dumps.

Or the command line:

```
python -m porter doctor
python -m porter port "D:\Games\NEO" -k "D:\keys\prod.keys" --install
```

The APK (`NeoTheWorldEndsWithYou.apk`) and the data pack (`NeoTheWorldEndsWithYou-data/`) appear in the game folder (`-o` picks another). `python -m porter install <App>-data` installs a port made earlier; with `--romfs-at <folder on the device>` the app reads the game's RomFS from a folder already there (an emulator's dump of the same version) instead of a copy in its files folder ([ARCHITECTURE.md](ARCHITECTURE.md#the-port), A RomFS already on the device). The first port takes a while (extraction, rewriting the game's code, translating its shaders); later ones reuse what has not changed. `python -m porter -h` lists the commands.

### Saves

`porter saves` does what the app's **Saves** tab does (see the README's [Saves](../README.md#saves)):

```
python -m porter saves list
python -m porter saves export "D:\Backups\Celeste"
python -m porter saves import "D:\Backups\Celeste\20261030-141200"
```

How the export folders are laid out: [ARCHITECTURE.md](ARCHITECTURE.md#saves).

### Vulkan driver

`porter driver` changes the Vulkan driver of a port already on the device, without the game's files: the app's APK is rebuilt with the library and installed over it (the saves stay), and its `env.txt` names it. The app must have been ported with `--vulkan-driver` once (that build carries libadrenotools, which loads the driver):

```
python -m porter driver "D:\Drivers\libvulkan_freedreno.so" --package org.nx2a.supermario3dworld
python -m porter driver --system --package org.nx2a.supermario3dworld
```

`--system` goes back to the device's own driver. How it works: [ARCHITECTURE.md](ARCHITECTURE.md#the-port).

### Host update

`porter host` brings a port on the device to this checkout's host (the runtime: Vulkan backend, front end, Horizon services), also without the game's files: its `libdmhost.so` is built again from the profile (a few seconds) and installed over the app (the saves stay):

```
python -m porter host --package org.nx2a.supermario3dworld
```

It refuses when the game's code on the device was rewritten with other rewriter sources than this checkout's; then port the game again from its files. The data pack is not changed. How it works: [ARCHITECTURE.md](ARCHITECTURE.md#the-port).

## Layout

- `porter/`: the port pipeline (`python -m porter`).
- `gui/`: the desktop app (`python -m gui`).
- `nxnative/`: our fork of nxnative (see [nxnative](#nxnative)), with the Android runtime in `nxnative/dm/android/` and the game profiles in `nxnative/dm/games/`.
- `packaging/`: the player's bundle (`python packaging/build_bundle.py`).
- `devtools/`: scripts for bringing up a game on a connected device (`devtools/device.json` names the device: `{"serial": "...", "package": "..."}`).
- `probes/`, `RESEARCH.md`: the feasibility study and the first on-device probes.
- `.githooks/`: the commit-msg hook (see [Commit messages](#commit-messages)).

## Adding a game

Each game is a profile in `nxnative/dm/games/<title ID>/` with an `android.tsv`: [ARCHITECTURE.md](ARCHITECTURE.md#adding-a-game). A finished port updates, in the profile's commit, the games table in the [README](../README.md#games) (its row, with a gameplay frame captured on the device in `docs/images/` and the tag of the nx2android version it was tested on) and [PORT_CHECKLIST.md](PORT_CHECKLIST.md).

## Releases and version numbers

The version is `0.MINOR.PATCH`. A release that only adds games and fixes bugs raises the patch version; a release with any other core change (a new feature or a change in behaviour) raises the minor version. The major version stays 0 while the project is early. A release checks only the games it adds or fixes, not the older ones; a break in an older game is fixed by a later patch release. The version is in `porter/__init__.py`, raised by a commit `chore(release): version <x.y.z>`.

## Building the bundle

```
python -m pip install -r requirements.txt pyinstaller
python packaging/build_bootloader.py      (Windows only)
python packaging/build_bundle.py
```

On Windows, `build_bootloader.py` compiles PyInstaller's launcher on this PC (it needs the Microsoft C++ Build Tools), because a few antivirus engines flag the prebuilt one every PyInstaller app shares. Run it again after installing or upgrading PyInstaller.

`dist/nx2android/` and `dist/nx2android-<version>-<os>-<cpu>.zip` (`windows-x64`, `macos-arm64`, `linux-x64`, `linux-arm64`): the app, the committed `nxnative/` and the install guide; no toolchains, keys or game files. Publishing a release builds the macOS, Windows and Linux zips on GitHub's runners and attaches them (`.github/workflows/release-macos.yml`, `release-windows.yml`, `release-linux.yml`, which builds both Linux cpus on Ubuntu 24.04 (`ubuntu-latest` for x64, so the system libraries the bundle carries are never older than glibc 2.39); `--clobber` replaces a zip of the same name, so do not upload them by hand). Run either workflow by hand (Actions → Run workflow) for a test build of a branch, kept as the run's artifact.

## Android motion

The default accelerometer shake detector still sends `DM_BTN_SHAKE` on devices
with or without a gyroscope. Ordinary gyro aiming below the shake threshold is
unchanged. `DM_MOTION_RAW=1` in `env.txt` opts in to keeping measured acceleration
and angular velocity during strong flicks instead of replacing them with the
fixed shake waveform. A game profile can supply the same setting through an
`env` row in `android.tsv` (see [Game profiles on Android](ARCHITECTURE.md#game-profiles-on-android)).
No profile enables it by default.

The opt-in requires an active gyroscope; without one, or with `DM_MOTION=0`, the
synthetic shake fallback remains. Explicit controller/keyboard `Shake` always
overrides raw motion. Physical Android validation of Super Mario Odyssey hand
and directional cap throws and Tears of the Kingdom bow aiming is still required
before enabling the opt-in in a profile. Host sensor tests verify routing and
axis preservation, not game recognition or device behavior.

## Tests

```
python -m unittest discover tests
```

The Vulkan-header harnesses need a discoverable NDK; set `ANDROID_NDK_HOME`
when it is not found automatically. They skip if no NDK headers are found.
The protected-pool harness uses real page protection
and a mutex/condition-variable rendezvous on macOS and Linux, handling each
platform's protection-fault signal. The zero-copy diagnostics fixture forces
slab and reuse-cache misses to exercise allocation/import failure cleanup; it
does not validate the slab or reuse-cache implementations.

`test_descriptor_hash` exercises the Vulkan descriptor hash and reuse path with
different C padding bytes but identical image fields. Image keys include each
array element's sampler, view and layout as explicit words; padding cannot force
another descriptor allocation. Reuse still compares the hash, layout and
submission epoch, without a full-content collision check.

`test_dmsv` checks both shader-bundle versions and the native reader. Publication
uses a unique staging directory beside the destination, flushes the file, verifies
every shader and reflection payload, then atomically replaces the destination.
Injected write, sync, readback and replacement failures leave the previous bundle
intact and clean up staging; concurrent writers cannot overwrite each other's
scratch files. The same contract applies to direct shader exports and CLI repacks.
This protects completed file publication, not power-loss durability of the parent
directory or semantic SPIR-V validation.

`test_vk_cube_views` compiles production image setup, view selection, both view caches and descriptor fallback. Square single-sample 2D arrays with at least six layers are cube-compatible, even when their total layer count is not a multiple of six. Selected cube-array views still require six-face groups, while a single cube uses six consecutive layers. Refused cube views emit one red diagnostic with image dimensions, creation flags, mip and layer ranges, then use the typed dummy. Tests cover seven/eight/fourteen-layer images, range boundaries, cache reuse and the diagnostic; host mocks do not qualify a physical Vulkan driver or establish the cause of the rendering reported in [issue #112](https://github.com/shirai91/nx2android/issues/112).

`test_vk_driver_cache` compiles the production driver-segment builder, cache lookup
and scratch allocator with mocked Vulkan allocation calls. A constant-hash test
stub checks that unequal segments never share a cached copy; it does not generate
a natural hash collision or establish a game symptom. Equal segments reuse the
existing copy without allocating. Each entry keeps a CPU copy of its live prefix
(up to 0x968 bytes; about 2.35 MiB of content across 1,024 entries). Candidate hits
compare 0x648 bytes for c0 or 0x968 for c2 in ordinary CPU memory, never the mapped
scratch allocation. If two banks hash alike, comparison includes the longer live
prefix and its implicit zero tail, preserving exact equality in both directions.
Scratch references are reusable only in their submission; recycling waits for
completion before the next epoch. GPU-written segments must remain outside this
immutable cache. Full descriptor-set key
equality is separate; these tests do not measure device performance.

Shader and translator reuse checks content receipts with relative output names,
sizes and SHA-256 hashes, including translator dependencies. The bundle takes
the fast path when size and modification time match on Windows. POSIX also
checks change time and file identity. Changed metadata falls back to hashing,
and matching contents refresh the recorded metadata. The Windows fast path can
miss a same-size edit that preserves the original modification time.
The shader key includes the translator's actual output bytes and the export helpers
`shader_vk.py`, `dmsv.py` and `shader_products.py`; unrelated Android tools do not
invalidate it. Inputs changing during generation stop publication. The changed key
and receipt format make existing ports translate once before reuse resumes.

The exporter records every expected shader (including geometry topology variants)
and failed key/reason in `shaders.dmsv.report.json`. It checks SPIR-V framing and
entry-point stage and requires reflection beside each emitted module. Valid
products from an interrupted batch are kept. Missing or invalid outputs get a
`[RED]` diagnostic. Before retrying integer vertex inputs, the exporter removes
the first pass's outputs to prevent reuse of an incompatible input interface.
`dmsv.write` reads each payload back and the runtime checks framing and reflection.
The porter compares the emitted keys and stages with the expected and failed
inventory and rejects an empty result before publishing the bundle, report or
receipt. An export with at least one valid shader can be reused with unchanged
inputs and products. The receipt records the failure count and the published
report's digest. A missing or changed report or changed input triggers a rebuild.
`test_shader_cache` and `test_shader_export_checks` exercise these contracts without
game files or a GPU.

Native harnesses discover Vulkan headers through `ANDROID_NDK_HOME` (or the
porter tool installation). Set it to an installed NDK when the SDK is outside the
usual locations. The POSIX probe uses mutexes and condition variables supported on
macOS and Linux. Protected-page tests handle both SIGSEGV and macOS SIGBUS and
keep their concurrent CPU writer checks; the zero-copy failure fixture supplies
the current allocator context while forcing cache misses to exercise every
allocation/import failure stage.

`test_vk_copy_back` exercises protected guest memory with the real dirty-page tracker: preserving independent CPU writes in the same cache line, successive submissions and snapshot initialization, cached/uncached and untracked pools, 200 partial/unaligned/overlapping range cases, and writes from another CPU thread. It also injects failed and partially successful bulk `mprotect` calls; a bulk unprotect failure stops the host with a `[RED]` message before publishing writable page states or continuing the copy. It also checks snapshot copy volume across clean and dirty 64 KB blocks, a partial tail, and full versus partial completion merges. These CPU tests do not establish correct rendering on a device.

`test_vk_memory_diagnostics` also checks per-buffer cached-memory compatibility, restricted memory-type masks, diagnostics disabled, and unchanged coherent memory selection. The candidate lookup is diagnostic; cached allocation requires the separate opt-in switch. `test_vk_cached_pools` injects creation, allocation, mapping and binding failures and verifies cleanup, coherent fallback, unsupported types, missing cache functions, default-off behaviour, device-loss handling and rejecting failed/timed-out GPU waits before readback. `test_vk_copy_back` also models separate CPU-cache/device bytes to check flush before GPU use, invalidate before merging, independent CPU writes, partial merges, clean submissions and fatal cache-maintenance errors. Cache-range tests check 5,000 generated ranges for both flush and invalidate, plus explicit boundary/overflow cases and multiple cache-atom sizes. Native readback tests cover empty/non-intersecting transfers, unaligned disjoint writes with independent CPU changes in the same atom, clipping at both pool ends, subsequent uploads/baselines, and full invalidation for uncertain writers. These mocks do not emulate an actual GPU driver.

With `DM_VK_MEM_DIAG=1`, `pool-flush`, `pool-invalidate` and `merge-scan` separate cache maintenance from result processing. Timing is per pool operation or merge range, not per submission; compare cumulative means and call counts. `merge-scan` includes changed-byte copying and page preparation. Scanned bytes differ from the existing changed-byte counter. Instrumentation is outside the 4 KB loop; logging and clocks still add overhead.

`copyback-breakdown` reports per-submission total copy-back time, summed non-coherent pool invalidation time, call count and logical bytes, remaining time, and cumulative means/percentage. The invalidation interval includes `pool_cache_sync` and its existing diagnostics, not only the Vulkan call; its logical bytes count requested intervals before atom rounding, excluding allocation padding. The remaining time includes lock waits, merges, iteration and measurement overhead. All inner phases overlap the outer total. Reports appear for the first three submissions and at most once per second afterwards, only with `DM_VK_MEM_DIAG=1`. No cache ranges or synchronization behavior change.

`test_vk_readback_ranges` checks conservative descriptor eligibility before descriptor memo returns and coverage when the transfer range list fills. Additional `test_vk_copy_back` cases check zero-work submissions, clipped disjoint transfer ranges, independent CPU updates, baseline reuse and full fallback for unknown writers, on both coherent and simulated non-coherent memory. A range limit is not proof of complete GPU-write tracking: new writers must use the conservative fallback unless their full destination is known.

The `sync-scan`, `sync-protect`, `sync-upload` and `sync-baseline` phases report per-sync totals for dirty-block loop residual, page protection, guest-to-buffer copies and baseline copies. Scan time includes loop and instrumentation overhead; it is not a pure dirty-query measurement. Timers run around uploaded blocks only, not every clean block. Allocation/metadata and flush costs remain in the outer `cpu-to-gpu` total; inner phases overlap that total and must not be added to it. Diagnostics can perturb performance.

`baseline-reason` reports per-sync and cumulative full-snapshot counts/MiB by mutually exclusive reason: `resync` (not synchronized yet, including newly registered tracking; takes priority over missing tracking), `untracked` (already synchronized but no tracking span), `incomplete-uncached` (tracked incremental sync with no completion merge since the last one, NVN CPU_UNCACHED flag) and `incomplete-other`. NVN CPU_UNCACHED is distinct from Vulkan HOST_CACHED. Incremental baseline block copies are excluded. The first three syncs and then at most once per second per thread report all categories. No memory contents are logged.

Eligible transfer-only completion also reuses NVN CPU_UNCACHED baselines. Native cases cover coherent and simulated non-coherent memory, empty/disjoint writes, independent CPU changes, incremental and clean snapshot volume, and fallback-to-eligible transitions. The old exclusion fails these tests. This does not change CPU_UNCACHED merge coverage for uncertain submissions; their baselines are reused too (no full snapshot of the pool at the next sync).

## Commit messages

[Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/), with commitlint's `config-conventional` types and limits:

```
<type>[(<scope>)][!]: <description>

[body, after one blank line]

[footers: Co-Authored-By: …, BREAKING CHANGE: <what breaks>]
```

- type: `feat`, `fix`, `perf`, `refactor`, `docs`, `test`, `build`, `ci`, `chore`, `style` or `revert`, lower case.
- scope: optional and free, the part touched: `vk`, `android`, `porter`, `gui`, `profile`, `status`, `release`…
- description: no full stop at the end; it may start with a capital (game and API names). Header, body and footer lines at most 100 characters (a lone URL may run longer).
- `!` before the `:` or a `BREAKING CHANGE:` footer marks a breaking change.

```
fix(vk): a transfer read where a run of rasterizer-discard draws ends (XC3 grass flicker)
feat(profile): Tales of Eternia Remastered playable on update 2.0.0 (v65536)
docs(status): bundle 8372959 ported Tales of Eternia Remastered from scratch
chore(release): version 0.1.1
```

`.githooks/commit-msg` refuses other messages (merges, reverts and `fixup!` commits from git pass). Turn it on once per clone:

```
git config core.hooksPath .githooks
```

The commits before 2026-10-28 predate the rule.

## nxnative

`nxnative/` started as a squashed git subtree of [seedds/nxnative](https://github.com/seedds/nxnative) (upstream 6034fca, 2026-10-24). Since 2026-10-29 it is our own fork: no `git subtree pull`, cherry-pick or merge from upstream, and the repository has no `upstream` remote (removed 2026-10-31): `origin` is the only one. Upstream's profiles are still a good reference when bringing up a game it has; read them on GitHub (`https://github.com/seedds/nxnative/tree/main/dm/games/<title ID>`) and write our own.

The Vulkan driver pipeline cache (`vk_pipeline_cache.bin` under `DM_ROOT`) now has a
versioned envelope containing the device, driver, API and UUID identity, a renderer
cache ABI, a bounded length and SHA-256 digest. A legacy raw cache is accepted only
when its first 32 bytes match Vulkan's version-one device and pipeline-cache UUID
header, then upgraded to the envelope on the next save even without new pipelines.
Legacy files have no digest or extended identity, so they retain only those older
compatibility checks until migration. Mismatched or corrupt envelopes start empty;
a driver rejection retries empty, and failure to create that cache is logged.
Pipeline creation marks the cache dirty even when its size stays unchanged. A
complete snapshot with the same size and SHA-256 as the last accepted load or
successful save skips publication. Other saves use one fixed `.tmp` file under the
I/O lock, so the next write reuses an interrupted save's staging file. Publication
requires successful writes and synchronization; failed or incomplete snapshots and
pending legacy migrations remain eligible for retry. `DM_VK_NOPCACHE` still means
start empty, while allowing newly built pipelines to be saved. Bump `PCACHE_ABI`
when changing the renderer's persistent cache contract.

## Optional native keyboard

`python -m porter port <folder> --native-keyboard` packages `org.nx2a.GameActivity` and its Unicode EditText dialog. Combine it with `--external-display` to select `org.nx2a.KeyboardExternalActivity`; external display alone keeps its controller keyboard. These opt-in activities require JDK `javac` plus SDK platform and build-tools; ordinary NativeActivity builds remain code-less and keep the controller keyboard. Installation and save requests resolve the installed package's launcher activity, including ports built before this option existed.

JNI passes ordinary UTF-8 byte arrays, which preserve supplementary characters. The guest's minimum and maximum lengths are UTF-16 units, and native output truncation preserves complete UTF-8 code points. The guest passes its request-local minimum length to a host pthread and joins it. That worker attaches to the VM, looks up and calls the Java method, and detaches on its own pthread stack. The bridge caches method presence or absence for the activity class. When the optional method is absent, the guest uses the overlay keyboard after the worker exits.

Cancel, pause and destruction release waiting callers without accepting text. Requests are serialized, and a concurrent request is cancelled with a log line. The native dialog takes activity window focus, so the regular Android window-focus callback reports the transition to the guest. The bridge supports text entry. Additional guest keyboard modes and candidate/update callbacks are outside its scope.

Run the asset-free emulator contract probe with:

```sh
python tests/android/keyboard_smoke.py --sdk "$ANDROID_HOME" --ndk "$ANDROID_NDK_HOME" --serial emulator-5554 --out /tmp/nx2a-keyboard
```

It builds through the production Java/dex packager, uses the real guest keyboard conversion and JNI bridge, and checks Unicode/UTF-16 output, minimum length, cancel, background/resume, repeated requests and bounded UTF-8 output. Add `--without-java` to verify the default APK's fallback. The probe installs only `org.nx2a.keyboardtest` and uninstalls it afterward; run it on a dedicated emulator. No game dump is needed. This probe does not qualify the combined external-display keyboard or physical-device gameplay.
