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

Both Vulkan atlas readbacks matched the deswizzled guest storage byte for byte. A registered-profile build produced the APK and data pack from separate base and update dumps.

On 2026-11-20, the updated 1.4.0 debug APK at `b3ed186` ran through the title, menus, opening, and initial hotel lobby on a Xiaomi 17 (Adreno 840, Android 17) using the device's Vulkan driver.

- The [lobby frame](../../../../docs/images/luigis-mansion-3.jpg) is a 1920×1080 game render resized to 1280×720. The carpet, floor tiles, and stairs render normally, and the game render has no Android gamepad overlay.
- SurfaceFlinger recorded 908 actual presents spanning 30.231 seconds in the stationary lobby, averaging **30.0 fps**. The median and 95th percentile present intervals were both 33.24 ms. The game profile targeted 60 fps and the default Android adaptive governor settled at 30 fps in this scene.
- With `--language system`, the phone's system locale selected `zh-Hans`. A temporary English (UK) locale for the test app selected `en-GB`. Clearing that app override restored `zh-Hans` on the next launch.

```text
[locale] Android zh-CN -> game zh-Hans
[locale] Android en-GB -> game en-GB
```

Later areas and long-session stability still need testing.
