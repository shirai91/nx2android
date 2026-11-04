Source: Zstandard v1.5.7, the latest stable release (2025-02-19), from https://github.com/facebook/zstd/releases/tag/v1.5.7 (the repository now answers as meta/zstd): `zstd-1.5.7.tar.gz`, SHA-256 `eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3`, which matched the `zstd-1.5.7.tar.gz.sha256` published with the release.

What was taken, unmodified:
- `zstddeclib.c`: the single-file decompressor, made by the release's own amalgamation step (`build/single_file_libs/README.md`), run from outside the unpacked tree: `python3 build/single_file_libs/combine.py -r lib -x legacy/zstd_legacy.h -o zstddeclib.c build/single_file_libs/zstddeclib-in.c`. No legacy formats, no assembly (`ZSTD_DISABLE_ASM`), error strings stripped (`ZSTD_STRIP_ERROR_STRINGS`), all set by `zstddeclib-in.c`. SHA-256 `5eb38abe0a7eea13674b312a006df72002d4192f0f0b4d09e6f03fb927a1f3d0`.
- `zstd.h`: `lib/zstd.h`, the API the host calls. SHA-256 `9b4bc8245565c98ccfc61c07749928b57e7c0f6fddb0530c4f6aa1971893d88b`.
- `zstd_errors.h`: `lib/zstd_errors.h`, which `zstd.h` includes (the error codes). SHA-256 `66a8c3f71d12ea6e797e4f622f31f3f8f81c41b36f48cad4f5de7d8bfb6aac0a`.
- `LICENSE`: the release's BSD license. zstd is dual-licensed BSD or GPLv2 at the user's choice; this repository takes it under BSD.

Use: the Android Vulkan backend decompresses the shader bundle's SPIR-V modules with it (`dm/android/gfx_vk_shader.c` `bundle_spv`; the bundle format in `dm/android/tools/dmsv.py`). `dm/android/build.py` compiles `zstddeclib.c` with its API hidden (`ZSTDLIB_VISIBLE` / `ZSTDERRORLIB_VISIBLE` empty, `-fvisibility=hidden`), so `libdmhost.so` exports none of it. The writer side is Python's own zstd (`compression.zstd`, or `backports.zstd` before Python 3.14), not this code.

To update: download a newer release, check its tarball against the published SHA-256, run the same `combine.py` line, copy `zstddeclib.c`, `lib/zstd.h`, `lib/zstd_errors.h` and `LICENSE` here, and update the version and hashes above and in `../README.md`.
