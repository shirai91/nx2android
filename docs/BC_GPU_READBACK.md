# Optional ordered readback for CPU BC uploads

`DM_VK_BC_GPU_READBACK=1` enables ordered GPU snapshots for buffer-to-texture
copies using CPU BC decoding. It is **off by default**: ordinary CPU-decoded
uploads retain their CPU-address source and add no queue submission or wait.
Native compressed-image copies are unaffected.

When enabled, an aligned source contained in one Vulkan pool chunk is copied
to coherent scratch memory and read after GPU completion. The helper records
the copied-pool readback barrier before ending the main command buffer.
It reuses completed command buffers rather than consuming split slots.

A full source range that crosses chunks, a misaligned transfer, or a failed
temporary CPU allocation uses the previous CPU-address decode with a one-time
diagnostic. The CPU fallback preserves compatibility; it does not guarantee
fresh GPU-generated blocks when a snapshot cannot be taken. Sources must still
fit the original guest pool's full address range.

Invalid regions/strides are diagnosed and skipped. Refused command buffers set
the submission's existing error flag, and the caller skips that upload. The
normal commit path drops the refused submission. Queue submission/wait failures
retain the backend's fatal error policy; incomplete GPU work is not reused.

## Device validation

Test the default path first, then enable the switch and repeat. On a GPU with
native BC, `DM_VK_X_BC_CPU=1` forces the fallback; this exercises the CPU path
but is not equivalent to testing Mali or an Adreno 619's driver behavior.
An executed snapshot logs `BC GPU snapshot: ordered readback completed` once.
Record this line, the driver identity, and both settings with the results.
Rendering alone does not establish that the new path ran.

The opt-in snapshot submits pending work and waits for queue completion while
holding the Vulkan queue mutex. This can delay presents. It is a correctness
experiment for GPU-produced blocks, not a general upload optimization.
