# Existing Android-parity DrawTexture replay

The source algorithm, GLSL fragment shader and public pixel oracle come from
`seedds/nxnative` commit `1134e0e75340bcc808ce79d67dfe00a9cf4ffe33` on the
Android parity branch. The source associates DrawTexture with Streets of Rage 4's
movie-to-scene draws. This test does not qualify commercial gameplay.

The production port reuses nx2android's pass tracking, descriptors, image allocator,
deferred destruction and sampler history. Its internal record adds `sseq`, like
ordinary texture binds. The source's rectangle/mip-copy algorithms are unchanged.
The initial source feature draws without blending and accepts single-sample 2D
float/normalized source views; integer/depth inputs and integer/decoded targets
remain unsupported. Later donor border/reduction emulation is outside this port.

Build the Vulkan host and the synthetic fixture (no game assets):

```sh
python nxnative/dm/android/build.py --game 0100000000010000 --ndk "$ANDROID_NDK_HOME" --out /tmp/draw-host --exe --jobs 8
python tests/run_vk_draw_texture_replay.py --ndk "$ANDROID_NDK_HOME" --host-build /tmp/draw-host --out /tmp/draw-fixture
```

The runner only builds files. To run on an explicitly selected Android device:

```sh
adb -s "$TEST_DEVICE" shell mkdir -p /data/local/tmp/nx2a-draw-texture
adb -s "$TEST_DEVICE" push /tmp/draw-fixture/draw-texture-replay /tmp/draw-fixture/shaders.dmsv /data/local/tmp/nx2a-draw-texture/
adb -s "$TEST_DEVICE" shell 'cd /data/local/tmp/nx2a-draw-texture && DM_NVN_ASYNC=0 DM_VK_RP=1 ./draw-texture-replay'
adb -s "$TEST_DEVICE" shell 'cd /data/local/tmp/nx2a-draw-texture && DM_NVN_ASYNC=0 DM_VK_RP=0 ./draw-texture-replay'
```

The copied public fixture checks two masked render targets, flipped fractional
rectangles, scissor clipping, nearest/linear sampling, disabled rendering and a
mip-feedback flip. The test adapter enables the existing `render_cond` recorder
option in its generated synthetic profile. It changes a sampler registration
between recording and submission to verify the existing sampler history rules.
A second public command list draws with an ordinary guest shader before and after
DrawTexture and checks that both targets, channel masks, viewport and uniform
binding survive. Only the synthetic guest shader keys bypass binary lookup.
The runner links the remaining production host objects, including the public
NVN lookup thunks and real Vulkan command replay. Its entry installs the normal
host dirty-page fault handler.

Validation: the donor oracle initially failed at target 0, pixel 2,1 (0 versus
128); both render paths pass after the port. The sampler reuse case failed at
118 versus 128 before connecting sampler history, then passed. All 192 native
sources built, and both paths passed on an Android 15 ARM64 SwiftShader emulator.
Physical GPU and commercial-game validation remain outstanding.
