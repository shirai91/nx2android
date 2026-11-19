# Luigi's Mansion 3 on Android

The [Android profile](android.tsv) is experimental for update 1.4.0 (`v327680`). It uses the existing NVN profile and shader tables.

## Build

Put your base game and update dumps in one folder as separate files. To use the Android system language at startup, run:

```text
python -m porter port "<game folder>" --version v327680 --language system
```

## Lobby rendering

The lobby binds prefilled 2048 × 2048 `RGB9E5` and `R8_UNORM` material atlases as `IMAGE` textures in a `CPU_NO_ACCESS` pool. Missing first uploads made the carpet, floor tiles, and stairs dark. The profile enables `DM_NVN_CPU_NO_ACCESS_IMAGE_FIRST_USE=1` to seed these images once. The Vulkan backend skips the upload when overlapping storage has been written by the GPU.

## Validation

The title, menus, opening, and early hotel gameplay were tested on a Xiaomi 17 (Adreno 840, Android 17) using a bring-up build. Both Vulkan atlas readbacks matched the deswizzled guest storage byte for byte. A registered-profile build produced the APK and data pack from separate base and update dumps.

Later areas, long sessions, and automatic locale selection on a physical device still need testing. The registered-profile APK was checked locally.
