# FIFA 21 Legacy Edition: experimental Android profile

The profile `01000A001171A000` reaches the main menu and a match on Xiaomi 17
(Android 17, Adreno 840), tested on 2026-11-17. It remains **experimental**:
one intermittent asset-reclamation crash is unresolved, and a complete match,
save/load cycle and longer stability run have not been qualified.

## Tested input

- Main NSO build: `480820c613eeea50b86aee580609bd89`.
- SDK NSO build: `cda31fe1e82ddeb67140a4486d882119`, SDK 9.6.0.
- Control metadata displays 1.0.7. The input XCI contains both base and update
  title IDs; separate update merging was not verified. Use the NSO build IDs to
  identify the tested code rather than relying on the archive's filename.

Supply your own game, keys and firmware using the normal porter setup. None of
those files, or the captured shader described below, are part of this profile.

## First run and runtime shader capture

The shared EA reader extracts 386 distinct shaders from this input's BIG/EASF/
chunkzip archives. One additional compute shader is generated at runtime. Without
it, stadium geometry renders but players and the ball appear black.

Use the existing capture workflow with a consistent test package:

```sh
python -m porter port ".gamedump/FIFA21" --debug --package org.nx2a.fifa21.test --install --device <serial>
```

Launch the app and enter a match. The host saves a shader missing from the bundle
in its external files directory's `captured_shaders/`. On the tested build the
missing key is `3d0b2d91c6de51fc`.

Run the same port command again with `--install` and the same package/device.
Porter pulls new captures into the local build's `android/captured_shaders/`,
includes them in translation and updates the installed data. The tested bundle
then contained 387 shaders; player uniforms, skin and the ball rendered correctly.
Keep the local captures for subsequent offline rebuilds.

## Bring-up changes

The guest layout reserves 5 GiB, with a 1.5 GiB alias region and a 256 MiB stack
region, leaving a 3 GiB heap region. The original guessed layout left only
128 MiB for the heap and rejected the game's `0xba400000`-byte heap request.

Shared runtime fixes handle Horizon's crash-report service switches and the NVN
sampler border setters, driver-arena usage queries and default zero subpixel
precision bias. Nonzero precision bias remains unsupported and logs a warning.
Android AAudio stream setup and disposal use the existing host big-stack entry:
the game's audio submit thread otherwise overflowed its guest stack during
Android's Binder/AIDL setup.

## Remaining limits

An intermittent SIGSEGV occurred in `EacGfx::Asset::System::DestroyRef` at
`main+0x239a4a4`, called from asset reclamation during world loading. It did not
reproduce in the subsequent observed run and one match restart, but its cause is
not established. This profile does not bypass reclamation or patch that function.
Temporary diagnostic observation gates used during investigation are excluded.

Frame rate varies by scene; no fixed 60 fps, energy-efficiency advantage or
comparison with an emulator is claimed. Fullscreen behavior was not changed.
Validation used the Android device above; other games and Apple platforms were
not retested for these changes.
