# Termux ARM64 Toolchain Compatibility

**Assessment date:** 2026-11-18
**Repository baseline:** nx2android 0.4.0
**Target environment:** Android 17, API 37, AArch64 (`arm64-v8a`)
**Status:** NDK ARM64 layout present; native compile/link validation pending.

## 1. Scope

This document records the compatibility assessment of the nx2android
development toolchain running natively in Termux on an ARM64 Android device.

The target is a native Android build. This is not an Android emulator running
inside another Android environment.

The assessment distinguishes between:
- tools detected by `porter doctor`;
- tool binaries and directories found on the device;
- a basic compiler syntax check;
- a complete native build and link.

The first three have evidence recorded below. A successful complete native
build has not yet been established.

## 2. Host environment

| Component | Observed value |
|---|---|
| Kernel | Linux Android, AArch64 |
| Android version | 17 |
| Android SDK/API level | 37 |
| ABI | arm64-v8a |
| Python | 3.14.8 |
| pip | 26.2.2 |
| .NET SDK | 10.0.113 |
| adb | 1.0.41, Android tools 37.0.0p1 |
| Free storage at assessment | Approximately 78 GB |

## 3. Termux tool inventory

The following tools were detected in the Termux environment:

| Tool | Observed version/state |
|---|---|
| clang | 21.1.8 |
| clang++ | 21.1.8 |
| ld.lld | 21.1.8 |
| CMake | 4.4.5 |
| make | Present |
| Ninja | Present |
| glslc | 2026.4 |
| Vulkan-Headers | 1.4.357.0 |
| SPIR-V Tools | 1.6.6.0 |

A basic compiler syntax check using the Termux Clang installation and
`--target=aarch64-linux-android31` passed.

This only establishes that the standalone Termux compiler can parse a trivial
Android-targeted translation unit. It does not establish that the nx2android
native build succeeds.

The Android build script selects compiler and shader tools from the NDK
directory. The standalone Termux Clang and glslc are not direct substitutes
for those NDK tools in the repository's normal build path.

## 4. Python dependencies and Porter doctor

`python -m porter doctor --json` reported:

| Check | Result | Notes |
|---|---|---|
| Python | Pass | 3.14.8; project minimum is Python 3.11 |
| Python modules | Pass | lz4, capstone, zstd |
| Pillow | Pass | Available |
| pycryptodome | Missing, optional | NSZ/XCZ dumps must first be decompressed to NSP/XCI |
| fontTools | Pass | Available |
| Android NDK | Pass | Revision 27.2.12479018 |
| .NET SDK | Pass | 10.0.113 |
| adb | Pass | Found in Termux |
| prod.keys | Fail, required | No valid configured file was found |
| Disk space | Pass | Approximately 78 GB free |

The missing `prod.keys` is a game-porting prerequisite, not an NDK compiler
layout problem. The configured key file must be supplied from the user's own
console before a game dump can be processed.

The optional missing pycryptodome dependency affects direct NSZ/XCZ dump
handling. It does not make the NDK unavailable.

## 5. Android NDK r27c

`porter doctor` located the NDK at:

`~/.config/nx2android/toolchains/android-ndk-r27c`

The reported revision is:

`27.2.12479018` (NDK r27c)

This matches the revision specified by the repository's toolchain checks.

The following ARM64 host-tool paths were confirmed to exist:

| NDK path | Result |
|---|---|
| `toolchains/llvm/prebuilt/linux-aarch64/bin/clang` | Present |
| `toolchains/llvm/prebuilt/linux-aarch64/bin/clang++` | Present |
| `toolchains/llvm/prebuilt/linux-aarch64/bin/ld.lld` | Present |
| `toolchains/llvm/prebuilt/linux-aarch64/sysroot` | Present |
| `shader-tools/linux-aarch64/glslc` | Present |

The corresponding `linux-x86_64` compiler, sysroot, and shader compiler
directories are also present.

The ARM64 host-tool directory contains LLVM utilities including `llvm-ar`,
`llvm-ranlib`, `llvm-strip`, `llvm-objcopy`, `llvm-readelf`, `llvm-nm`,
`llvm-objdump`, `llvm-size`, `llvm-strings`, and `llvm-symbolizer`.

The NDK's x86_64 LLVM resource tree contains:

`lib/clang/18/lib/linux/libclang_rt.builtins-aarch64-android.a`

The existence of this archive is confirmed. The ARM64 compiler wrapper's
resource-directory behavior and successful use of this archive during a
native build still require validation.

## 6. Repository build behavior

The Android native build is implemented in:

- `nxnative/dm/android/build.py`
- `docs/DEVELOPMENT_GUIDE.md`

The build script resolves the NDK from `ANDROID_NDK_HOME`, or from its
repository-relative default when that variable is not set. The Porter workflow
can also pass the NDK path found by `porter.toolchain.find_ndk()` explicitly.

The build script selects `linux-aarch64` when the expected ARM64 NDK clang
path exists. Otherwise, its Linux fallback is `linux-x86_64`.

The script uses the NDK's compiler and shader compiler paths. Its Android
target defaults to API level 31 (`aarch64-linux-android31`).

The native build includes the Android host/runtime components and associated
native libraries. A successful tool discovery check does not prove that all
these sources compile or that the final executable and shared library link.

The repository's ARM64 development guide describes an NDK r27c ARM64 host
overlay using native ARM64 LLVM 18 tools, the NDK sysroot, and a native ARM64
`glslc`. It also documents the LLVM builtins requirement and the resource
directory consideration for compiler builds that do not carry the Android
builtins archive themselves.

The discovered ARM64 overlay paths satisfy the build script's initial
host-directory selection checks. The complete compiler-wrapper and link
behavior remains to be tested.

## 7. Compatibility conclusion

The current evidence supports the following conclusions:

1. Termux is running on the required AArch64 Android host architecture.
2. Python and the required Porter Python modules pass `porter doctor`.
3. The installed NDK is the expected r27c revision.
4. The ARM64 NDK compiler, linker, sysroot, and shader compiler paths exist.
5. The build script should select the ARM64 NDK host-tool directory based on
   the discovered clang path.
6. A trivial Android-targeted syntax check passes with standalone Termux Clang.
7. No successful nx2android native compile/link has yet been recorded.
8. Game processing remains blocked until a valid `prod.keys` path is configured.

**Overall:** the Termux environment has the expected NDK r27c ARM64 host-tool
layout. Native build compatibility remains pending an actual project compile
and link test. Do not treat the basic Clang syntax check or `porter doctor`
success as proof of a completed native build.

## 8. Remaining validation

- [x] Confirm Android AArch64 host architecture.
- [x] Confirm Python version and required Porter modules.
- [x] Confirm .NET SDK and adb availability.
- [x] Confirm NDK r27c revision.
- [x] Confirm ARM64 clang, clang++, ld.lld, sysroot, and glslc paths.
- [x] Confirm ARM64 LLVM utility binaries are present.
- [x] Confirm the Android-targeted standalone Clang syntax check.
- [ ] Inspect and validate the ARM64 clang/clang++ wrappers and resource
      directory configuration.
- [ ] Run a project native compile using the installed NDK.
- [ ] Run a complete native link and verify the expected outputs.
- [ ] Configure a valid user-owned `prod.keys` file.
- [ ] Install pycryptodome only if direct NSZ/XCZ input support is needed.
- [ ] Validate a game-specific profile and complete the first Android port.

## 9. Evidence and reproducibility

The recorded results came from the following device-side checks:

- `python -m porter doctor --json`
- NDK `source.properties` revision inspection
- Existence checks for ARM64 and x86_64 NDK compiler and shader-tool paths
- Inspection of the ARM64 NDK host-tool directory
- Search for `libclang_rt.builtins-aarch64-android.a`
- A trivial Clang syntax check targeting `aarch64-linux-android31`

Repeat these checks after changing the NDK, compiler wrappers, Termux packages,
or repository revision.

## 10. Source references

- Android build script:
  https://github.com/shirai91/nx2android/blob/main/nxnative/dm/android/build.py
- Development guide and ARM64 Linux NDK setup:
  https://github.com/shirai91/nx2android/blob/main/docs/DEVELOPMENT_GUIDE.md
- Porter tool detection:
  https://github.com/shirai91/nx2android/blob/main/porter/toolchain.py
