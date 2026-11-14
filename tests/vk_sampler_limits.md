# Ported Android sampler limits

Production formulas come from `seedds/nxnative` commit
`bbd6424db8669e09317725c82ba94bb75f4acc6c`, retained in Android parity
`1f01e28c87da5233dfa14fb31fabf1535df33013`.
`sampler_for` is adapted to nx2android's existing `make_sampler`; its feature gate
uses the existing `ff.features` passed into device creation.

The port clamps LOD bias to both the guest range and device limit, clamps mip LOD
ranges, and uses whole-number anisotropy within the guest/device limits only when
supported. It preserves current sampler caching/history, filters, addressing,
comparison, border colors and reduction chaining.

```sh
python -m unittest discover -s tests -p test_vk_sampler_limits.py
python tests/run_vk_sampler_limits.py --ndk "$ANDROID_NDK_HOME" --out /tmp/sampler-probe
adb -s "$TEST_DEVICE" push /tmp/sampler-probe/sampler-limits /data/local/tmp/sampler-limits
adb -s "$TEST_DEVICE" shell chmod 755 /data/local/tmp/sampler-limits
# Run each mode 0..2 and each case 0..8:
adb -s "$TEST_DEVICE" shell /data/local/tmp/sampler-limits 1 0
```

The fixture compiles the actual production sampler function, parameter type and
feature-limit assignment, with an observer at `vkCreateSampler`. Cases include
the donor's four bias/anisotropy requests and every mip-filter mode. Twenty-four
of the 27 cases fail on unchanged main. The host version captures create info;
the Android version checks it and lets the real driver create every sampler.
Modes cover ordinary limits, smaller advertised limits, and a device created
with anisotropy disabled. There are no production test switches.

Historical validation before the review follow-up: all 27 real sampler creations
passed on the task's Android 15 ARM64 SwiftShader emulator. That run checked
accepted sampler state, not filtered pixels or commercial gameplay. The revised
head has host-harness and NDK build validation; the emulator run has not been
repeated, and physical GPUs remain unqualified.
