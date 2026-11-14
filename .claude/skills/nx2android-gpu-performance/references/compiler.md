# Optional driver compiler inspection

`runtime/pipeline_compile_probe.c` creates a Vulkan device and pipelines,exports
`VK_KHR_pipeline_executable_properties` statistics/internal representations,and
never creates a command pool or submits GPU work. The loader accepts an explicit
driver filename;it also supports the existing adrenotools HAL loading path. It
requires executable diagnostics and dynamic rendering support. Lack of support
is a limitation,not permission to swap drivers.

Build with the project's NDK and Vulkan headers;keep generated binaries,case files
and SPIR-V under `.gamedump/<App>/<run>`:

```text
<clang> --target=aarch64-linux-android31 -std=gnu11 -O2 -I<repo>/nxnative/dm/third_party/vulkan-headers/include <skill>/runtime/pipeline_compile_probe.c -ldl -o <run>/compile-probe
```

Stage the executable,authorized current driver/support libraries,case file and
validated modules in a unique device scratch directory. Check all hashes. Execute:

```text
compile-probe <remote-directory> <driver-filename> <remote-case-file>
```

The probe does not install/restart/modify the game. Remove only its own temporary
device executable after completion;retain local IR/statistics for reproduction.

## Case format

Whitespace-delimited:case count(max16),then each case:

```text
label vertex.spv fragment.spv NV NF NA NB NC DEPTH_FORMAT DEPTH_TEST DEPTH_WRITE DEPTH_COMPARE TOPOLOGY SAMPLES
# NV vertex descriptor rows,then NF fragment descriptor rows:
binding VkDescriptorType descriptorCount
# NA vertex attributes:
location binding VkFormat offset
# NB vertex bindings:
binding stride inputRate
# NC color attachments:
VkFormat colorWriteMask
```

Comments shown above explain the schema;the numeric case file must contain no
comments. Supply actual Vulkan enum values,not NVN enums. The vertex descriptor
set is0and fragment set1. Translate reflected dynamic UBO descriptors consistently
with the host's layout. Preserve original partner stages,bindings,attributes,
formats,sample count,depth state and masks across paired cases. Instance divisors
other than1,geometry/tessellation,blend equations,stencil,logic and exact raster
variants need an extended adapter or an in-app capture;do not claim exact live
pipeline equivalence from this reduced schema. Raster defaults are back-face cull,
clockwise front face and no blending. Do not compare reconstructions with different
defaults as a shader-only change.

## Evidence requirements

- Validate SPIR-V with the toolchain's `spirv-val` for the runtime target,verify
  reflection/interfaces and inspect both main and binning executables.
- Do not assume executable index0/1means VS/FS;read stage/name metadata.
- Preserve original driver identity. Executable text and statistic names are
  driver-specific. Count equality does not guarantee code equality.
- Register/wave limits are compiler capacity estimates,not measured occupancy.
  Stall-cycle estimates are not GPU time. Absence of STP/LDP in a reconstruction
  does not prove every live pipeline is spill-free.
- Prove algebra/address/FP equivalence independently;avoid reassociation,precision
  reductions,NaN/denormal changes or descriptor-layout differences hidden in a
  supposedly simple address candidate.
- Fewer instructions can cover little actual scene work or leave the binning path
  unchanged. Require same-scene whole-frame A/B and dynamic visual validation
  before calling it a retained optimization.
