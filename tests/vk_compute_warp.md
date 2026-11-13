# Existing logical32 compute warp port

The four `spv_warp*.py` modules are copied from nxnative Android parity commit `1f01e28c87da5233dfa14fb31fabf1535df33013`, with only flat-import adaptations. Their existing history includes `c6e63d666`, `1eb775a5a`, `b8a67d6b0`, `5616788e1`, `d60835615`, `2577f558b` and `b4cac58ec`. PR #362 supplies their existing Module/reflection helpers (and depends on #355).

Supported fixed-size compute shaders use logical Maxwell 32-lane IDs, lane masks, shuffle, boolean votes and ballot, backed by workgroup shared storage. Converged guest shared-memory warp barriers widen both scopes. The donor's optional optimizer-assisted pass can lift register-only conditional exchanges; all control-flow, shared-memory budget, operand and decoration safeguards are retained. Unsupported modules stay byte-identical at this pass. No native-width runtime contract, graphics-stage lowering or new fallback is added. Original Switch only.

The common exporter runs offsets, compute warp lowering, then half/numeric lowering. The porter supplies its selected NDK's `spirv-opt`, and includes its contents and all four pass files in the export receipt. Standalone `shader_vk.py` accepts `--spirv-opt` and otherwise uses PATH; without an optimizer, conditional lifting remains disabled, as in the donor. Existing per-shader export failure handling remains unchanged.

## Reproduce

Put the NDK shader tools on PATH and run `python -m unittest discover -s tests`. The 18 copied donor tests validate actual SPIR-V and cover convergence, calls, loops, early returns, side effects, duplicate/escaped builtins, specialized/local dimensions, scratch budgets and shared barriers. Export/cache tests verify the published bundle and tool/source invalidation.

Build the production host and translator using the instructions in `vk_resource_pools.md`. Then:

```sh
python tests/run_vk_compute_warp.py --ndk "$ANDROID_NDK_HOME" --host-build /tmp/pool-host --translator /tmp/pool-translator/dmsh_xlate.dll --out /tmp/warp128 --subgroup 128 --converged-only
python tests/run_vk_compute_warp.py --ndk "$ANDROID_NDK_HOME" --host-build /tmp/pool-host --translator /tmp/pool-translator/dmsh_xlate.dll --out /tmp/warp32 --subgroup 32
```

The builder does not contact a device. On an authorized test device, copy `compute-warp`, `shaders.dmsv`, `cases.txt` and the `.expected` files into a dedicated directory. Run there with `DM_ROOT` set to that directory and `DM_VK_NO_ZEROCOPY=1` on the emulator. `--native` builds a negative control with only this lowering disabled.

The guest instruction generators and independent expected-value calculations come from the donor's shuffle, vote, mask and predicated fixtures. The target adapter uses its existing 256-byte buffer alignment, support/uniform reflection, DMSV packaging and recorded compute dispatch. Each output is surrounded by guards. Coverage includes 32/64/128 invocations, multidimensional groups, a partial 36-lane group, all shuffle modes, repeated scratch reuse, predicate validity, inverted votes, and all lane masks.

## Results and remaining limits

On Android 15 ARM64 Lavapipe with physical subgroup size 4, nx2android's unchanged default `subgroup=128` translation passes 138 converged cases / 13,920 expected words plus guards. The donor's `subgroup=32` setting passes all 144 cases / 14,736 words plus guards. The unlowered default-translation control fails its first shuffle case. The host suite passes 360 tests, with 9 skipped. The device fixture links the previously verified 192-source parent host build; this PR changes no native runtime source.

**Conditional-shuffle limit at the target default:** the six donor predicated Maxwell fixtures remain unchanged when translated with `subgroup=128`. That translator places a subgroup lane-ID load inside the conditional arm, which the donor's register-only lift deliberately rejects. On the four-lane emulator the first such retained case fails readback. `--subgroup 128` without `--converged-only` reproduces this known limitation. This port does not change the target's translation default or broaden the donor's safe-to-move opcode set. The same fixtures lower and pass with the donor's 32-lane setting. Specialized sizes, unsupported collectives, excess/unknown shared storage and unsafe divergent work retain their existing native path too.

No physical GPU, performance or commercial-game qualification is claimed. Tears of the Kingdom has known warp-using shaders, but a new playable-game count has not been established by these synthetic checks.
