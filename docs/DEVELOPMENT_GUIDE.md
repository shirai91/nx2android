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

`dist/nx2android/` and `dist/nx2android-<version>-<os>-<cpu>.zip` (`windows-x64`, `macos-arm64`): the app, the committed `nxnative/` and the install guide; no toolchains, keys or game files. Publishing a release builds the macOS and Windows zips on GitHub's runners and attaches them (`.github/workflows/release-macos.yml`, `release-windows.yml`; `--clobber` replaces a zip of the same name, so do not upload them by hand). Run either workflow by hand (Actions → Run workflow) for a test build of a branch, kept as the run's artifact.

## Tests

`test_dirty_protection_failures` checks failed and partially successful read-only protection and defragmentation restore calls. The host stops before publishing a guessed page state; successful protection, restore and subsequent page-fault writes remain covered.

```
python -m unittest discover tests
```

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

The native regression tests `tests.test_dirty_publication` and `tests.test_vk_texture_dirty` cover in-flight generation publication, writes during page protection, GPU-owned texture retention, and retries after failed or partial packaged uploads.

The main-based integration retains the upstream pool index insertion/removal, baseline completeness policy, subgroup handling and timeline fallback. It adds GPU ownership marking on cached storage-image views and skips descriptor-key work only when descriptor caching and stage memoization are disabled. Native regressions cover sampled/storage hits, null-image views, enabled cache hits, disabled cache writes, and cache re-enabling.

Zero-copy import now finishes before replacing guest pages, releases imported allocations after failures, rejects truncated FD packets, and stops on unsafe guest mapping restoration/remap failures. These failure paths are covered by `test_vk_memory_diagnostics`, `test_vk_recv_fds`, `test_vk_pool_restore`, and `test_vk_pool_remap`.

`DM_VK_ZC_FD_SCAN=1` is an experimental startup-only payload selector, separate from diagnostics. The TB373FU sends a read-only `gralloc_extra` FD first, the requested-size dma-buf second, and a metadata memfd third. The selector requires exactly one read-write FD whose size covers the pool and whose proc target identifies a dma-buf; missing/inaccessible metadata or ambiguous candidates reject zero-copy before mapping guest pages. Initial mapping and read-only remapping share this selector, whose mode is fixed at first use. With the switch off, the original first-FD path is unchanged. Native mocks exercise both handle orders, padding, invalid/short/read-only/metadata handles, ambiguity, initial mapping and remapping, cleanup and rejected selection. This heuristic is not an Android native-handle layout guarantee, and neither successful selection nor mapping establishes CPU/GPU cache coherence on Mali; device testing is required before enabling it by default.

`DM_VK_ZC_HOST_BARRIER=1` independently tests GPU-to-host visibility: the existing MEMORY_WRITE to HOST_READ barrier is emitted at the existing partial and final submission sites even without copied pools. Its default is off, preserving the old condition; mixed/copied submissions still receive a single barrier. Activation is logged once and the mode stays fixed until restart. This adds no waits, CPU cache synchronization or external ownership transitions, so it cannot establish complete dma-buf CPU-access correctness. `test_vk_copy_back.test_readback_dependency` checks scopes/access masks, zero-copy with the switch off/on, mixed/copied behavior and restart-only policy.

`DM_VK_ZC_INIT_LOCK=1` brackets a newly allocated AHardwareBuffer's initial CPU upload with `AHardwareBuffer_lock(CPU_WRITE_OFTEN)` and `AHardwareBuffer_unlock(NULL)`. The returned CPU pointer is used for the copy; a NULL unlock fence waits synchronously, before the fixed guest mapping or Vulkan import. Lock failure, a missing CPU pointer or unlock failure rejects the new buffer while the original guest pages remain intact. Default off retains the original temporary-mapping copy. Tests cover copy-before-unlock ordering, the returned pointer, synchronous unlock, failure cleanup, no fixed mapping after failed initialization and the unchanged default. See the [Android lock/unlock contract](https://developer.android.com/ndk/reference/group/a-hardware-buffer). This is an initialization fix only: later guest CPU reads/writes still need an independently justified synchronization model. Whole-pool invalidation while other CPU threads write is not a safe replacement for actual CPU access boundaries.

`DM_VK_ZC_SERIAL=1` is a diagnostic wait at submission begin, after the slot is recycled and before command replay. It waits for the prior submission's completion thread, not only its GPU signal, without turning zero-copy pools into shadow pools or adding CPU cache maintenance. Copied pools retain their single existing wait and upload; a failed wait stops rather than proceeding to pool use. The mode is fixed at first submission and logs once. `test_vk_zero_copy_serial` executes the actual policy block with mocked completion and verifies default zero-copy overlap, opt-in waiting, copied/mixed behavior, wait-before-upload and fail-stop handling. This is not a race fix: guest workers continue running and may already have written a buffer before the wait. An improvement suggests timing/lifetime involvement; no improvement cannot rule out races or cache hazards. Compare the same motion with all other settings unchanged; serialization may substantially lower FPS.

`DM_VK_COPY_POOL=serial:size:flags` is a startup-only diagnostic selector (serial decimal or `*`, whole-pool byte size and NVN flags hexadecimal). An unchunked pool takes the existing copied-pool allocator when size and flags match, plus serial if it is not `*`. `*:1000000:22` copies the entire 16 MiB/0x22 candidate class regardless of creation order; it is not a unique vertex/index identity and can select several pools, including ones whose role is not yet known. This avoids the observed failure where serial 94 became a 0x24 pool on restart. The older exact selector remains available for controlled comparisons. Default off and invalid selectors preserve existing backing choices. MemoryPoolInitialize supplies flags before Android chooses a backing through a weak host hook; other hosts retain mtl_pool_add followed by vk_pool_set_flags, and virtual runs use the old no-flags entry. Selected pools are logged even beyond the normal first-24-pool cap. Other pools retain their existing zero-copy/fallback policy, and copied pools retain their existing cache, incremental upload and readback behavior. No mapped pool is converted live. Confirm candidate roles and copy volume in each fresh trace; broader copying may cost memory/FPS. Native tests run the actual selection/creation path for exact matches, a reordered mixed size/flag population where serial 94 no longer matches, invalid/wildcard inputs, overflow, chunk rejection, restart-only configuration and the non-Vulkan fallback. On-device visual correctness and performance remain unverified.

Advance Wars retains unresolved intermittent building flicker during Capture on TB373FU and grass flicker at battle entry on S25. The S25 grass symptom also reproduces with the unmodified local main `5181e97` native host using the same game assets and configuration; these integration changes do not claim to fix either issue.
