# Evidence and interpretation

| Evidence | What it measures | What it does not establish |
|---|---|---|
| CPU replay counters | Host recording/descriptor/pipeline wall-clock work | Whole game CPU cost or GPU execution |
| Completed 64-slot history | Sum of completed game-submission timestamp spans per tagged frame | Isolated shader busy time;can include command-feed gaps |
| Full pass rows | TOP-to-BOTTOM render/dispatch command/dependency intervals | ALU/texture/bandwidth hardware-stage split |
| Presentation markers | Initial clear/transitions,blit,overlay,rotation in a separate GPU submission | CPU acquire waits,compositor or physical scanout |
| Pipeline statistics | IA/VS/clipping/FS invocation counts of actual submitted draws | Time shares,visible pixels,utilization or speedup limits |
| Compiler executable data | Actual driver instructions/registers/variants for a specified pipeline | Measured occupancy or whole-frame speedup |
| Draw state | Bound resources,masks,depth/stencil,programs and attributes | Which shader branch/sample executes for each pixel |
| Sensors | Accessible sysfs values/clock caps at sample times | An enforced lock or guaranteed fresh temperature |

## Complete accounting

Reject reports ending before positioned anchor+one whole report period. Check all
render ordinals and their dimensions/formats against the current scene trace.
Separate render and compute numbering. Normalize each row as
`mean_ms_per_call * actual_calls / reference_frames`; do not charge a rare compute
dispatch once per frame. A compute ordinal can mix different kernels/dimensions.

Use matching report windows for render+compute versus game-submission means.
Report the difference as **unassigned**. It may include copies,barriers,CPU command
supply and reporting/rounding effects;do not assign invented subcosts. Keep a
negative residual as a discrepancy requiring investigation,not zero or "saved"
work. Same-frame spike records are a useful independent consistency check.

Presentation has separate batches. CPU and GPU overlap. Do not add means from
different windows to a median or claim a sum is elapsed frame time. Report ordinary
baseline and intrusive timing results separately,even when they happen to agree.

## Local attribution

- Internal BOTTOM markers may serialize/change tiled execution. Time moved to
  `render_begin` is not a measured attachment transfer or CPU API call cost.
- Full chain cost precedes shader priority:tiny local windows can be useful leads
  without explaining the complete frame. Cover the remaining budget explicitly.
- Same shader/depth signature does not freeze geometry,uniforms,resource contents,
  optional effects or animation. Join only exact same-frame counters when needed.
- A pass with few fragments can still incur vertex/binning,attachment and dependency
  costs. Early-clipped/query geometry can have zero FS invocations and nonzero work.
- A sampled descriptor sharing a bound image is a feedback **lead**. Verify mip,
  layer,actual shader paths and write masks. Offset equality across different pools
  is not identity. Descriptor memoization can suppress repeated log lines.
- Unused color masks do not permit skipping depth/stencil,alpha,occlusion queries,
  storage writes or draw preparation. Retained image contents may be needed later.
- Fewer native passes/draws/instructions can lose performance through worse overlap,
  binning or scheduling. Preserve negative/null results instead of promoting them.

## Acceptance

Require a repeatable same-build same-scene whole-frame improvement or a clearly
qualified local result;review all reversal windows and clock variation. Validate
moving scenes and relevant scene loads/restarts before retaining a rendering
change. Report no-power/no-temperature proof unless measured over an appropriate
duration. A short static check cannot establish sustained60FPS or thermal targets.
