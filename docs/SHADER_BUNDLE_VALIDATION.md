# Runtime shader-bundle validation

The native reader accepts DMSV versions 1 and 2. Before publishing a bundle's
mapped pointers it checks the complete index, strictly increasing unique keys,
known NVN stage numbers, payloads outside the header/index, and both outer and
inner reflection bounds. The binding count and constant-data length must fit
inside that entry's reflection record, using wide arithmetic before any pointer
calculation. Version 1 cannot contain compressed modules.

Each module is still copied/decompressed lazily. The reader checks zstd length
and checksum, SPIR-V size/alignment, magic, reserved schema word, version 1.0–1.3
(the translator's Vulkan 1.1 contract), and every instruction's nonzero length
and containment. An invalid module is logged and omitted without rejecting its
otherwise valid neighbors. A malformed index/reflection rejects the whole bundle.

`python -m unittest tests.test_dmsv` compiles and runs the actual native reader.
Mutated fixtures cover index ordering, unknown stages, header overlap, truncated
reflection, overflowing binding/constant counts, zero/overrunning instructions,
version/schema mismatches and compressed corruption, alongside v1/v2 round trips.
The random codec fixture has valid instruction framing but deliberately is not
a semantic shader.

These checks adapt the bounded product contract in nxnative; they do not replace
full `spirv-val`, check every opcode's operands, or establish device capability
support. Export-time semantic validation and the broader resource/capability
schema remain separate work. The binary file format is unchanged.
