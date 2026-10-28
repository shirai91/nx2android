# Optional custom-driver and performance controls

All controls are off by default and contain no game IDs. They do not unlock
a game's frame limit or replace per-build frame-rate patches.

- `DM_RENDER_BIG_CORE=1`: the presenting/replay thread queries its allowed
  CPU set and selects the highest-capacity CPUs in that set. Missing capacity
  files, invalid values and denied requests leave ordinary scheduling in place.
  This is startup-only. Threads created afterward can inherit the affinity;
  two did in the Motorola capture. It is not a perfectly isolated intervention.
- `DM_VK_DRIVER=turnip`: load a supplied app-local Turnip through adrenotools.
  Unset or `system` keeps the system loader. An explicitly requested missing
  custom driver is an error rather than an unnoticed driver switch.
- `DM_GPU_PERF=1`: with the patched adrenotools below, preload the small KGSL
  power hook **inside the custom driver's namespace**. It adds
  `KGSL_CONTEXT_PWR_CONSTRAINT` to context creation and requests
  `KGSL_CONSTRAINT_PWR_MAX` for the returned context on the same fd. There
  are no guessed context IDs, root calls, vendor perfd dependency, dummy
  submissions or global GPU power-control disable.

The exact Motorola kernel examined was `8b49a4a7343d`. It activates the
context constraint for submitted work, tracks owner/timestamp/expiry, and
retains thermal and maximum limits. Expiry is checked by the kernel's power
policy; this is not a promise of immediate frequency reduction on pause.
The physical GPU is shared, so other GPU consumers can see a higher clock
while this context's requirement is active. The hook only covers the
reviewed driver's three-argument ioctl calls; a driver-native implementation
is preferable if that contract expands. Other kernels and GPU vendors are
not covered by this device result.

## Build the optional libraries

Supply your own matching Turnip build and use an isolated adrenotools source
checkout. No driver binaries are distributed here. This recipe was checked
with adrenotools `8fae8ce254dfc1344527e05301e43f37dea2df80` and Android NDK
28.2.13676358; the ordinary upstream host toolchain remains unchanged.
Recheck the patch when using a different adrenotools revision.

Example shell recipe, with `repo`, `build_root`, `ndk` and `turnip_so` set
to your own paths (`build_root` must be a fresh work directory):

```sh
git clone --recursive https://github.com/bylaws/libadrenotools.git "$build_root/adrenotools-src"
git -C "$build_root/adrenotools-src" checkout 8fae8ce254dfc1344527e05301e43f37dea2df80
git -C "$build_root/adrenotools-src" submodule update --init --recursive
git -C "$build_root/adrenotools-src" apply "$repo/nxnative/dm/android/adrenotools-kgsl.patch"
cp "$repo/nxnative/dm/android/driver/kgsl_power_hook.c" "$build_root/adrenotools-src/src/hook/"
cmake -S "$build_root/adrenotools-src" -B "$build_root/adrenotools-build" \
  -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-31 -DANDROID_STL=c++_static \
  -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_root/adrenotools-build" --parallel 4
mkdir -p "$build_root/driver-libs"
cp "$build_root/adrenotools-build/libadrenotools.so" "$build_root/driver-libs/"
cp "$build_root/adrenotools-build/src/hook/"*.so "$build_root/driver-libs/"
cp "$turnip_so" "$build_root/driver-libs/libvulkan_freedreno.so"
export DM_VK_DRIVER_LIBS="$build_root/driver-libs"
```

Run the ordinary porter command with that environment. Its APK stage passes
the supplied `.so` files to the existing APK builder's `--extra-so` input;
an explicitly supplied empty/missing bundle is rejected. CMake, a build
tool and Git are needed only for this optional loader build. The recipe
has not been exercised on Windows.

Then add the three controls above to the app's existing `env.txt` and
restart it. Do not change every game/device profile by default. The GPU
flag alone does nothing when the ordinary system driver is selected.
Rollback is to remove the controls and restart; no save restoration is needed.

## Checks and evidence

Host tests compile the actual affinity, loader and ioctl-hook code against
small mocks. They cover denied/missing capabilities, startup-only selection,
driver-load errors, ABI/payload, unrelated calls, unsupported creation flags,
permission refusal, interruption and errno preservation. NDK compilation
checks the Android ABI separately. See [device evidence](../../../../docs/ANDROID-PERFORMANCE-FREAPSY.md)
for what has and has not been measured on hardware.
