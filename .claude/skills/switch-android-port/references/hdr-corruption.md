# Intermittent HDR corruption and retained pixels

Use this workflow when block corruption appears after moving the view and then
persists while still, or when a few bright pixels contaminate bloom. It is not a
general image-correctness detector: finite wrong colours, absent geometry and
incorrect lighting may have no NaN or infinity.

## Preserve a natural incident

1. Save a screenshot, current log, process ID, driver and active switches before
   probes. Ask the user to stop when the error is visible. Never inject confirm
   input into the user's save to reproduce it.
2. Determine whether the error exists before tone mapping. Ordinary PPM output
   clips HDR above 1, so apparently solid white is not evidence of overflow.
   `DM_VK_PASSDUMP_RAW=1` preserves float32 RGB in `pass_NN.ppm.f32`.
3. Use a pass list to identify the main HDR target, then inspect colour and
   postprocessing inputs separately. `DM_VK_TRACE_PASS=-2` logs shader changes
   within each pass. Pass numbers can change while moving or opening a menu;
   match dimensions, formats, target identity and shader keys as well.

The switches are DM_CTL `env` lines; see [switches.md](switches.md). A bounded
example after locating the intended pass (replace 30 with the observed ordinal):

```text
env DM_VK_PASSDUMP_SIZE=2560x1440
env DM_VK_PASSDUMP_PASS=30
env DM_VK_PASSDUMP_FMT=122/78
env DM_VK_PASSDUMP_TARGET=0
env DM_VK_PASSDUMP_DEPTH=0
env DM_VK_PASSDUMP_CROP=0,1024,2560,384
env DM_VK_PASSDUMP_RAW=1
env DM_VK_PASSDUMP_FRAMES=16
env DM_VK_PASSDUMP=1
```

Format 122 is B10G11R11 unsigned float. The `/78` qualifier describes the next
colour-format state in this example, not a general requirement; use the actual
trace. A 2560x384 crop over 16 images copies roughly 60 MiB and exports 180 MiB of
RGB32F. Whole-resolution, multi-target or depth captures can be much larger. The
96-dump storage cap counts images, not presents; a frame may have many matches.

Pull the capture's fresh log segment and its files into a unique directory under
`.gamedump/<App>/`. Preserve them BEFORE rearming: filenames are reused. A raw file
left by an older float/depth dump can coexist with a new non-float PPM under the
same name. Interpret it only when the current log reports a supported float
format and its dimensions and byte length match. Raw depth is single-channel;
HDR colour is three-channel. Colour crop coordinates do not crop depth readback.

The reusable offline analyzer validates that evidence and reports non-finites,
finite extrema/mean and bounded example coordinates:

```powershell
.\.venv\Scripts\python.exe devtools/hdr_probe.py dumps .gamedump/<App>/case/dump.log --output .gamedump/<App>/case/hdr.json
```

Coordinates are relative to each captured image; add the logged crop origin.
Missing/truncated HDR payloads are errors, not clean results. Non-float formats
are skipped even if an old `.f32` exists. Keep one capture batch per directory/log.

## Capture the transition, not only the retained result

Prepare recording and bounded HDR batches before telling the user to demonstrate.
Verify that the native recorder is actually running and that an initial clean
HDR batch completed. Only then notify the user. Capture enough image area for
source coordinates to move; a clean strip says nothing about the rest of the frame.
Save the last finite batch and first non-finite batch, including present IDs.
Report capture gaps explicitly: finite -> large finite -> infinity proves growth,
but a missing interval does not identify the first bad write.

Do not equate a visible trigger with its cause. A menu may merely hold the camera
at the angle where earlier colour survives. Preserve the distinction between a
natural reproduction and a deliberately seeded diagnostic marker. Announce any
probe that visibly changes colours, filtering, depth or missing draws before it
is enabled. Recordings/readbacks alter workload and may hide timing hazards.

## Find the depth writer and corresponding material

After an ordinary HDR dump identifies a poisoned pixel, request one frame:

```text
env DM_VK_PIXEL_HISTORY=2560x1440:1009,1334
```

The default-off tool records one pixel after logical draw and clear records for
the matching single-sample B10G11R11 target 0, up to 4096 events. It uses one
64 KiB readback buffer plus bounded metadata. The file is `pixel_history.tsv`;
wait for the completion log, then pull it before the next history capture.
Unsupported formats/sample counts/coordinates are not captured. Ordinary pass
dumps cannot run concurrently. `kind` is 1 colour clear, 2 depth/stencil clear,
3 array draw, 4 indexed draw. `prepared=1` does not prove a GPU draw was issued.

```powershell
.\.venv\Scripts\python.exe devtools/hdr_probe.py history .gamedump/<App>/case/pixel_history.tsv --output .gamedump/<App>/case/history.json
```

Inspect depth changes, retained HDR words and candidate matching vertex/index
buffers, counts, instances and base vertices in the same frame. Confirm the
candidate with normal traces: viewport/scissor, input attributes, uniforms,
textures/samplers, alpha threshold/discard, depth comparison, bias and blending.
Equal addresses/counts alone are not proof of equivalent geometry. An unchanged
colour may result from depth rejection, discard, colour masks or the same value.

Per-draw pass cuts and transfer reads are intrusive. First check that the final
poisoned pixel and screenshot remain wrong under history capture; if the probe
cures it, investigate the pass/copy perturbation instead of trusting the history
as an untouched frame. Never use these runs for performance comparisons.

## Verify a repair and clean up

- Prefer same-process original -> candidate -> original reversal at an unchanged
  view. A diagnostic clear can erase retained colour irreversibly; disappearance
  alone then cannot identify the first cause. Global clears, NaN sanitization and
  threshold/filter changes are not substitutes for the semantic repair.
- A live `DM_VK_SPV_ALT` override disappears on restart. Verify the process ID and
  actual alternative-module load messages after a restart before classifying a
  recurrence. Alternative modules/pipelines are cached: changing files or the
  directory does not guarantee reload; coordinate a restart for a different set.
- Validate changed SPIR-V and reflection. Compare exact-source old/new compilers
  so unrelated source/facts/export changes are not accidentally deployed. A
  translator repair needs regenerated shader data; an APK-only update may keep
  the old bundle. Preserve the original data and the user's latest saves.
- Close the loop with ordinary shader data after restart, then user-controlled
  movement, turns and scene transitions. State the tested duration and scenes;
  compile checks are not gameplay checks.
- Reset RAW, crop, pass/FS/format/size filters, frame count and pixel history;
  restore temporary shader overrides/skips/extra clears, stop owned recorders,
  and verify the original startup configuration. Keep evidence under `.gamedump/`.

Odyssey case (2026-11-12): a depth alpha shader retained the perspective multiply/
reciprocal round trip while the material used direct interpolation. The existing
compiler rule missed reversed outer operands. Inconsistent alpha coverage left
depth with stale colour; a later multiply amplified it to infinity and bloom
spread NaNs into blocks. Two natural source coordinates passed reversible tests,
and user movement/prompt checks passed after deploying regenerated ordinary
shaders. This does not establish a cause for unrelated ground-grid artifacts.
