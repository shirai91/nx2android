# PR343 vertex-input qualification

These complete tables list the integer-declared `(shader key, location, component)`
pairs on upstream main `e5a4703` and the interface-preserving revision. The columns
are equal for every row; no pair was added, removed or changed. They contain only
derived shader/interface metadata, not game code, binaries or keys.

| Game | Exported modules | Vertex modules | Main pairs | Revised pairs | Changed input signatures | Changed reflection |
|---|---:|---:|---:|---:|---:|---:|
| Super Mario Odyssey 1.4.1 | 27,996 | 7,418 | 1,306 | 1,306 | 0 | 0 |
| Breath of the Wild 1.9.0 | 19,592 | 8,931 | 1,354 | 1,354 | 0 | 0 |

- [Odyssey exhaustive input set](odyssey.tsv)
- [BOTW exhaustive input set](botw.tsv)
- [Complete comparison counts and bundle hashes](comparison.json)

The exporter first establishes the original typed interface with address
canonicalization disabled, using main's exact inference rule and existing profile
hints. It then fixes those attributes for the optimized translation. Every
optimized module is checked for equal locations/components, scalar types,
signedness, widths and vector/matrix/array shapes. An unsupported or changed
interface fails export. Stale translator controls and unsuccessful retries are
also rejected. This does not depend on enumerating gameplay vertex formats.

The old proposal inferred six extra Odyssey signed-int inputs at location11:
`3c3435a62e05975b`, `4e7d155a4c3c2155`, `aa34bcfa5f592ec0`,
`baa8765fd5e4dfc9`, `e66c6a3dd539f1be`, `f581ad177e89e31f`.
Those additional declarations are absent in the revision. Retained pipeline
caches covered five with R8G8_UINT, stride2/offset0/divisor0; the first key had no
runtime record. That partial observation was not a universal compatibility proof,
so widened inference was removed instead. All six revised modules pass
`spirv-val --target-env vulkan1.1`.

Both complete exports preserve upstream #316. Only vertex payloads differ from
main; all other stages and all binary reflection bytes are identical. Compared
with the old full PR candidate, Odyssey differs in only those six VS, and BOTW
is byte-identical (SHA256 `3c92b88da935aa2b9aada0e305ba4c5ee51de056a6f285d7690fd3ef416aaa2b`).

Validation:96 shader-related Python tests passed on Windows, including the
production integer-address harness, legacy inference, export ordering, interface
drift, stale translators, failures and offline NuGet/timeouts. The chain header
was regenerated and checked against the production version function. The separate
upstream chain fixture had four Windows symlink-privilege errors and eight passing
tests; it is not reported as a successful complete suite.

This evidence establishes unchanged input interfaces for these exports and the
exporter's per-module contract. It does not establish that every pre-existing
main vertex format is valid or replace regression testing of address semantics.
The extra compile and signature work is offline; no per-frame format guard was
added. The six revised Odyssey payloads have not been newly exercised in gameplay.
