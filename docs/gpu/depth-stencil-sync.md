# Depth/stencil synchronization: implementation and validation

Integration base: master `66637da025133ff21d8bff00e7810fda9ec27f8a`.
Investigation/benchmark baseline: `3065a7e3a85633d6b3faffd4bcefa6c171b5cbbb`;
the intervening master changes are HID-only and the audited GPU sources are identical.
Branch: `fix/depth-stencil-sync`. No changes to Geometry PR #349, shader-compiler,
presentation PR #350, Texman PR #244, NCA, HID, scheduler, NVDEC or queries.

## Meaning and historical evidence

This investigation adopts the user's identification of **DS Sync as Depth/Stencil
Synchronization**. Technically, the implemented responsibilities are preservation
of depth/stencil values between guest memory and Vulkan images, independent
aspect transfers, coherent packed-layout aliases, and correctly ordered clears
and attachment/texture consumers. A barrier alone cannot convert guest texel
encoding or preserve a stencil value that was never transferred.

The public 2024 Discord quotation supplied earlier does not expand the acronym or
identify a register/API. No independently accessible primary source was found
proving that its unfinished feature meant precisely the defects fixed here.
The [official 2023 recap](https://strato-emu.github.io/posts/recap-2023/) describes
texture variants and on-demand synchronization/UsageTracker, but not “DS Sync”.
This limitation is historical attribution, not uncertainty about the reproduced
depth/stencil defects below.

Original Skyline already attempted multi-aspect transfers on 2021-12-08:

* [aa32f6b0](https://github.com/strato-emu/strato/commit/aa32f6b017ee83ca670578c477998b5df136f2a2): depth/stencil Texture support.
* [bd6cd005](https://github.com/strato-emu/strato/commit/bd6cd0056c8211b334d7a525034c8603b6cd4297): multi-aspect readback.

Both used separate aspect regions with the same initial buffer offsets. Current
master retained that behavior. Therefore the fork had DS paths, but their data
representation was not correct for combined formats. `git show`, `git blame` and
`git log --all --regexp-ignore-case --grep='depth.*sync\|stencil.*sync\|DS Sync'`
provide the reproducible public-history trail; no unpublished upstream feature
was assumed to exist or copied.

## Proven defects

1. **Overlapping aspect planes.** `GetBufferImageCopies` reset bufferOffset to
   zero for each aspect, and used packed guest byte strides for both. Upload
   interpreted interleaved packed words as independent planes; readback allowed
   stencil writes to overwrite depth output. This is data corruption even when
   Vulkan validation does not reject the region list.
2. **Wrong guest texel widths.** `D32FloatS8Uint` was four bytes instead of eight;
   `S8Uint` was four bytes instead of one. Those widths also drive swizzle sizes,
   guest mapping/stride calculations and staging bounds.
3. **Unaligned tiny mip offsets.** Single-aspect D16/S8 mip regions could start
   at offsets not divisible by four. Packed guest mip layout needs conversion
   to aligned Vulkan buffer regions as well as correct aspect selection.
4. **Linear serialization defects.** Upload copied the whole surface once per
   layer, overrunning buffers for array textures. Linear readback copied guest
   bytes into the host buffer instead of host data into guest memory.
5. **Stencil load clear missing.** Full DS/stencil clears updated loadOp but
   not stencilLoadOp. A later lower-index color/DS clear could also shrink
   clearValues and discard a previously recorded higher attachment's clear.
6. **Opposite D24 packed orders shared a backing.** The same Vulkan format does
   not make the two guest byte layouts interchangeable. Exact-mapping manager
   reuse ignored stencilFirst, and DS layouts were excluded from the existing
   reversible alias authority mechanism.

Tests reproduced overlapping offsets/data corruption, wrong metadata, upload
ASan overflow, reversed readback, missing stencil clear, truncated clear values
and the incorrect D24 compatibility predicate before the corresponding fixes.

## Representation and implementation

The [Vulkan copy specification](https://docs.vulkan.org/spec/latest/chapters/copies.html#copies-buffers-images-addressing)
requires separate buffer representations of depth and stencil. D24 depth uses
four-byte X8_D24_UNORM_PACK32 with depth in the low 24 bits; D32 depth uses four
bytes; stencil uses one byte. Copy regions select one aspect each. This differs
from image barriers: without separateDepthStencilLayouts, combined DS barriers
must continue to include both aspects. No such optional feature is required here.

Guest formats are mapped by hardware TIC numeric values, not similarly named
format enums in another emulator:

| Strato guest format | TIC | Packed guest encoding | Conversion |
| --- | --- | --- | --- |
| D24UnormS8Uint | 0x2b | depth low24, stencil high8 | Mask depth; extract high byte |
| S8UintD24Unorm | 0x29 | stencil low8, depth high24 | Shift depth right8; extract low byte |
| D32FloatS8Uint | 0x30 | float32 then stencil low8 of second word | Preserve float bits; extract byte4 |

`depth_stencil.h` converts **in place** in the existing staging allocation. Depth
is compacted forwards on upload and expanded backwards on readback. Stencil
starts after the entire packed surface; each mip's stencil start is aligned to
four bytes. This avoids an additional temporary surface or buffer allocation.
Mip-major/layer-contiguous ordering matches the existing deswizzler. Single-aspect
formats use bounded memmove only when mip padding actually changes an offset.

Ignoring per-mip alignment, staging is 5 bytes/texel for D24S8 and 9 for D32S8;
the latter deliberately leaves room for safe in-place expansion. These are
transient/cached transfer buffers, not extra Vulkan images. The upper unused
24 bits of the D32S8 second word become zero on readback. They are not emulated
as preserved raw data, and that format is excluded from lossless byte-alias
authority. D32 float payload bits and all meaningful stencil/depth bits survive.

`SynchronizeHostImpl`, `CopyToGuest` and download staging allocation use the same
layout as `GetBufferImageCopies`. Normal image copies still use the existing
single upload/readback commands, now with disjoint aspect regions. Existing
image-scoped barriers, fence cycles, resource retention, dirty tracking and
guest traps are retained. Reordering three barrier initializers only makes
their designated initialization valid in standard C++20; Vulkan fields are unchanged.

For exact-mapping native 2D block-linear D24 aliases, TextureManager now rejects
direct reuse across opposite packed orders and enters the existing AliasGroup
registration/retry/reconciliation path. The latest GPU writer alone encodes the
guest bytes; a stale representation lazily reuploads the correct interpretation.
Existing memory leases protect registration, generations reject stale reads and
conflicting simultaneous writers, and repeated settled reads reuse backings.
Same-order lookups keep their fast path. No global alias policy is rewritten.

CommandExecutor scheduling is unchanged. `RenderPassNode` now sets both relevant
load operations for a full DS view clear and preserves higher-index clear values.
Maxwell's existing partial-aspect/partial-rectangle vkCmdClearAttachments route is
retained. Existing sequential attachment-to-sampled dependencies already include
early/late fragment test stages, so no speculative barriers were added.

## Reference comparison

* Yuzu `7ffac53c9e67fa18d067114a8eb3e77c74bba68f`,
  [surface.h](https://github.com/yuzu-emu-mirror/yuzu-mainline/blob/7ffac53c9e67fa18d067114a8eb3e77c74bba68f/src/video_core/surface.h),
  independently confirms S8=8 and D32S8=64 bits. Its Vulkan texture cache handles
  feedback barriers by leaving the render-pass context.
* Ryujinx `534f92506bf8c6aba29bb86917bf10954a360d8e`,
  [FormatTable.cs](https://github.com/kotx/Ryujinx/blob/534f92506bf8c6aba29bb86917bf10954a360d8e/src/Ryujinx.Graphics.Gpu/Image/FormatTable.cs)
  and Vulkan/OpenGL FormatConverter.cs establish the numeric TIC crosswalk and
  opposite packed orders. Vulkan TextureView tracks bound/storage hazards, but
  its buffer-copy code reduces combined DS to depth only: it is not a complete
  stencil roundtrip implementation to import verbatim.
* Eden `14da4482a4668928bad43bb67ef12f96b9c9d972`,
  [vk_texture_cache.cpp](https://github.com/eden-emulator/mirror/blob/14da4482a4668928bad43bb67ef12f96b9c9d972/src/video_core/renderer_vulkan/vk_texture_cache.cpp),
  transforms aspect regions but also duplicates offsets in that transformation.
  Its texture cache differs architecturally; that method alone is not proof of
  a correct packed upload/readback representation.

The patch follows Vulkan's representation rules and Strato's existing authority
mechanism rather than copying an emulator backend wholesale.

## NieR log and causal limits

The supplied Adreno 650 log `emulation(20261016-220007).log` reports 04584 for
descriptor binding4/index0 overlapping framebuffer attachment0. It supplies view
handles, but not their format, VkImage, aspect, mip/layer ranges, or preceding
image history. Attachment index0 does not establish whether it is color or DS.
Consequently the specific overlap cannot be attributed to depth/stencil from
this evidence, and this patch does not claim to resolve that same-draw feedback.

Unsupported geometry subgroup operations (06343), disabled imageCubeArray
(01004), and device loss remain independent. No subgroup, CubeArray, shader
replacement or device-fault implementation is changed. Earlier fault diagnostics
contained no actionable address/vendor record. The thread first observing device
loss does not identify the command that caused it.

## Tests and reproducibility

The five focused suites are:

```
python3 tests/gpu_depth_stencil/run.py
python3 tests/gpu_depth_stencil_guest/run.py
python3 tests/gpu_depth_stencil_clear/run.py
python3 tests/gpu_depth_stencil_vulkan/run.py
python3 tests/gpu_depth_stencil_alias/run.py
```

Their READMEs document dependencies, facades, environment overrides and negative
controls. Region/helper and guest serialization tests use ASan/UBSan; clear
methods use UBSan and real Vulkan types. The guest suite executes the actual
CPU serialization bodies and production pitch-copy functions in 20 cases,
checking independent expected planes/guest data and unchanged row/layer padding.

Headless Vulkan ran on llvmpipe LLVM17.0.6, requesting Vulkan1.1 without optional
DS device features. It executes production transfer commands, four mips/three
layers, both packed D24 orders, D32S8, single-aspect D16/S8 odd-mip layouts,
repeated uploads, isolated aspect/subresource clears and actual compute sampling
immediately after an attachment write. Shader SPIR-V passes spirv-val for Vulkan1.1.
Validation and synchronization validation report zero errors.

The alias suite uses the real production lookup/group/coherence/transfer bodies
and real Vulkan resources: alternating writers, exact bytes, stale guards,
conflicting writes, memory pin/release and repeated cached reads pass. Settled
reads add zero allocations/submissions. An independent reviewer reran all five
suites and found no production blockers. Existing pitch layout (310 roundtrips),
24 BC decode cases, occlusion, conditional rendering and DRAW_TEXTURE suites pass.

These host facades do not emulate the complete Android process, memory trap or
Maxwell/frontend lifecycle. They are distinct from an actual game test.

## Performance boundaries

No queue/device idle, sleep, timeout, new runtime thread, global lock, global
barrier, auxiliary shader/pipeline or unconditional submission is introduced. Packing
runs only when a real DS guest/host transfer is needed, after the existing dirty
state checks; GPU-resident same-backing draws do not execute the CPU conversion.
Color transfers retain their representation and normal texture caches remain.

The new D24 alias bridge uses the existing resource-specific readback/upload
fallback only when an incompatible interpretation is stale. Those operations
replace incorrect shared-backing reuse; they are not free. Repeated settled
reads are explicitly tested to add no work. No Android FPS guarantee can be
inferred from that structural property or from a CPU microbenchmark.

The benchmark runner documents exact compiler, sample counts, warmup and CPU
affinity reproduction. CPU measurements are recorded separately from FPS,
frametime, GPU submissions, shader compilation and Android memory usage.

GCC 13.3, `-O2`, affinity CPU0, seven samples of 5,000,000 iterations after
100,000 warmup iterations; sequential **master/fixed/fixed/master** run order:

| Run | Revision | Color region generation median (ns/call) |
| --- | --- | ---: |
| 1 | master 3065a7e3 | 67.8416 |
| 2 | fixed | 65.5255 |
| 3 | fixed | 108.7600 |
| 4 | master 3065a7e3 | 111.8030 |

No consistent regression appears in the paired region-generation results.
The large drift on this shared host prevents a speedup claim or a useful bound
on Android overhead. Shorter initial measurements were noisy as well; the final
region implementation deliberately reuses the original emitter rather than
duplicating it for DS formats. The fixture uses a standard vector in both builds,
so absolute numbers are not the production small-vector allocator cost.

The final fixed runs measured the **new** 1920x1080 D24 unpack+pack conversion at
4.55552 and 5.59751 ms per roundtrip (seven samples of ten roundtrips, no timed
allocation). This is a real CPU cost when guest/host serialization is required;
it is not incurred on every draw or GPU-resident frame. Master has no correct
equivalent conversion to benchmark. The runner's `--ref` deliberately changes
only region generation/metadata, so its helper timings must not be misreported
as correct master DS serialization. No FPS/frametime improvement is claimed.

Shader compilation time, total pipeline count, real CPU/frame and Android memory
were not measured. Shader compiler/cache implementations are unchanged; that is
structural evidence, not a replacement for gameplay measurements.

## Remaining coverage and device tests

No Moto G100, Qualcomm proprietary or Turnip game run was possible here. NieR is
**not confirmed fixed**, nor are FPS/frametime or cross-game regressions ruled out.
Compare master and this commit with identical driver/settings, save/scene and
replacement files; record multiple equal-length warm runs and cold shader-cache
runs separately. Measure FPS, mean/p95/p99 frame time, CPU/frame, memory and
submissions/fence waits/presentation timing when the device tools expose them.

Same-draw sampled/writable attachment feedback, general partial-overlap aliases,
opposite-order DS aliases outside the admitted exact native 2D block D24 layouts,
MSAA resolves and a new unsupported-format conversion fallback are not implemented
by this patch. Their existing limitations must not be described as fixed. Native
format support still depends on the Vulkan physical device. D32S8 unused padding
is explicitly not a lossless reinterpretation source.

PR #349 remains separate and unmerged; this master-based branch does not silently
include its Geometry changes. PR #350's independently proven producer fixes also
remain separate. No merge is performed. Opening the Draft PR triggers Android
ARM64 PR Build plus host DS validation. CI completion, APK download and device
testing are left to the user; no job polling or artifact download is performed.
