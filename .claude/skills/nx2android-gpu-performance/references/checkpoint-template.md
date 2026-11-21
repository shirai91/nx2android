# Local progress checkpoint template

Write this under `.gamedump/<App>/<run>/`,not inside the reusable skill.

```json
{
  "status": "capture finished;normal controls restored",
  "objective": "user's actual performance and quality goal",
  "identity_manifest": "absolute path to verified session/build record",
  "last_verified_pid": "historical only;recheck before memory access",
  "scene": "kingdom/map/position/view and screenshot path",
  "conditions": "resolution,quality,driver,split,clock mode,observed clocks,temperature trend",
  "retained": ["only separately verified improvements and fixes"],
  "rejected": ["candidate,measurement and rejection/correctness reason"],
  "confirmed": ["measured fact with artifact path"],
  "hypotheses": ["unproved attribution and required next check"],
  "next_action": "one concrete bounded action",
  "raw_evidence": ["full report","state traces","A/B cases","compiler output"],
  "restoration": "persistent hashes,live controls,startup timing status,latest saves checked",
  "active_loops": "none or exact owned process/session to stop"
}
```

Result summary:lead with the whole-frame budget,then largest logical chains and
their semantic evidence. State changed variable,build identity,frames/windows,
median/tails,all reversals and confounds. Distinguish positive retained gains,
local-only improvements,null results and correctness failures. Save the full row
table/CSV;do not replace it with only the top few passes. Keep scene-specific
ordinals and reproduction notes in the local checkpoint,not in generic code.
