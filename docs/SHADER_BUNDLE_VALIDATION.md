# Runtime shader-bundle validation

The native reader accepts DMSV versions 1 and 2. Before publishing a bundle's
mapped pointers it checks the complete index, strictly increasing unique keys,
known NVN stage numbers, payloads outside the header/index, and their file bounds,
including space for each reflection header. Opening reads only the header and
index; payload and reflection pages stay untouched. Version 1 cannot contain
compressed modules.

Each module is still copied/decompressed lazily. The reader checks zstd length
and checksum, SPIR-V size/alignment, magic, reserved schema word, version 1.0–1.3
(the translator's Vulkan 1.1 contract), and every instruction's nonzero length
and containment. An invalid module is logged and omitted without rejecting its
otherwise valid neighbors. On that shader's first lookup, `vk_shader_q` also
checks that the binding count and constant-data length fit its reflection record,
using wide arithmetic before calculating pointers. A malformed reflection record
is logged and cached as missing for that shader, leaving valid neighbors usable.
Only a malformed index rejects the whole bundle.

`python -m unittest tests.test_dmsv` compiles and runs the native bundle reader and
the lookup path through reflection parsing, stopping before Vulkan module creation.
Mutated fixtures cover index ordering, unknown stages, header overlap, truncated
reflection, overflowing binding/constant counts, zero/overrunning instructions,
version/schema mismatches and compressed corruption, alongside v1/v2 round trips.
Protected payload pages verify that opening does not read SPIR-V or reflection;
bad reflection counts omit only that shader and remain cached on repeat lookup.
The random codec fixture has valid instruction framing but deliberately is not a
semantic shader.

These checks in `nxnative/dm/android/gfx_vk_shader.c` follow the bundle layout in
`nxnative/dm/android/tools/dmsv.py` and the reflection writer `refl_bin` in
`nxnative/dm/android/tools/shader_vk.py`. They do not replace full `spirv-val`,
check every opcode's operands, or establish device capability
support. Export-time semantic validation and the broader resource/capability
schema remain separate work. The binary file format is unchanged.
