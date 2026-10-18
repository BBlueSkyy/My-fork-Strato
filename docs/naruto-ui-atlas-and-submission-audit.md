# Naruto Storm 4 UI: atlas sampling and PR #356 audit

Base: PR #356, `310b2c32bb1542b52a0c56bd7e5c4a9a2793517d`.
Diagnostic PR #357 remains separate and unchanged. No merge authorized.

## Captured first draw: what is demonstrated

The supplied 74-entry capture and matching log contain the first indexed UI
draw: 30 indices, two instances, R16G16 signed-scaled position and RGBA8 UNORM
vertex color, BC3 512x128 sampled atlas, and source-alpha blending. There are
47229 changed RGB pixels between `pre_00` and `pass_01` (1120x630).

An offline interpreter of the captured SPIR-V uses actual VS CBUF2 and both
vertex streams. Vulkan supplies Z=0 and W=1 for the two-component integer
position attribute before the shader's signed-to-float conversion. The
previous -0.34..+0.34 UV estimate omitted W=1 and is invalid. Reconstructed
vertex UVs are approximately -0.000388..0.680065 and -0.001961..0.595702.

Rasterizing the 20 triangles with the GPU-readback BC3 atlas, normalized
linear/clamp sampling and the captured fragment/blend math reproduces all
705600 RGB pixels within 2/255 of `pass_01`; mean absolute channel error is
0.013075/255, and changed-pixel mask intersection-over-union is 98.677%.
The small differences are consistent with software versus fixed-function
filtering/rounding. The same reconstruction using unnormalized coordinates
changes zero pixels, versus 47229 in the capture. This demonstrates how the
captured atlas produces the black shapes; it does not establish the original
guest atlas bytes or its raw TIC layout.

## Normalization hypothesis corrected

TSC word1 bit25 is an override, not a Boolean normalized-coordinate state:

| TSC floatCoordNormalization | TIC normalizedCoordinates | Effective mode |
| --- | --- | --- |
| 0 | 1 | Normalized |
| 0 | 0 | Unnormalized |
| 1 | either | Forced unnormalized |

NVIDIA defines 0 as `USE_HEADER_SETTING` and 1 as
`FORCE_UNNORMALIZED_COORDS`. Thus the captured TSC=0 does not demonstrate
a mismatch. Eden/Yuzu and Ryujinx-derived code can retain normalized Vulkan
samplers and divide guest rectangle coordinates by texture dimensions in the
shader. Strato's missing TIC-to-Color2DRect selection and TSC force override
remain separate unsupported cases, not proven Naruto causes, and are not
changed here.

Direct Vulkan unnormalized samplers also require equal min/mag filters,
nearest mip mode, zero LOD bounds, clamp U/V, no anisotropy or comparison,
single-layer/single-mip 1D or 2D views, and restricted shader sampling
operations. The captured implicit-LOD operation is another reason not to
flip the sampler Boolean blindly. Existing sampler cache keys, filtering,
mip policy and shader modules remain unchanged.

## Proven generic texture-unit layout defect and correction

TIC sampled surfaces auto-size oversized Y GOB blocks to cover the actual
height in format-block rows. Single-mip TIC construction previously forwarded
the declared Y block directly into deswizzling. For a synthetic 512x128 BC3
surface, declared BH16 must sample with BH4: 128/4 compressed rows /8 GOB rows
= 4 GOBs. Using BH16 selects separated 16-pixel strips, with the first wrong
packed byte at offset64, and eventually reads padding/neighbouring content.

`common/textures.cpp` now caches the existing declared layer stride and
allocation size, then reduces effective sampled Y for single-mip block-linear
2D color TICs with non-interleaved Z and base array layer zero. The allocation remains262144 bytes in this fixture; packed pixels are
65536 bytes. Array/cube layer advancement remains declared. 1D, 3D, depth/stencil, nonzero base-layer and interleaved-Z sampled layouts remain unchanged
until their differing-layout coherence is supported. Multi-mip, pitch,
render-target and DMA layout code remain unchanged. This avoids treating all binding points as
auto-sizing, which the hardware documentation explicitly distinguishes.

This is a demonstrated generic defect, with the same strip mechanism visible
in the captured atlas. **The Naruto TIC and pre-upload guest source were not
captured. BH16 in that exact draw is not confirmed. Device validation is
required before calling this a Naruto fix.**

The TextureManager also routes exact overlaps with different Y layouts through
its existing alias registration/retry path. This submits pending writer work
before guest bridging and shares generations so later writes invalidate the
cached sampled view. Reusing the old backing or creating an ungrouped image
would lose those writes; a regression exercises the production routing and
writer lifecycle.

## PR #356 synchronization audit and corrections

1. `FenceCycle::WaitSubmit` shared the mutex held by `Wait` during
   `vkWaitForFences`. It could therefore block until GPU completion, despite
   intending to wait only for queue submission. Submission publication now
   has its own mutex/CV and release/acquire atomic state. The completed-submit
   fast path never acquires the GPU completion mutex, including the race
   between checking pending state and submission notification.
2. `SubmitInternal` checked a texture's predecessor after
   `SynchronizeHostInline`. A CPU-dirty staging upload replaces that cycle
   with the current one; the wait then skipped the predecessor. It now pins,
   deduplicates, waits and chains the old cycle before staging, preserving
   post-sync lifetime chaining as well.
3. Clean/direct buffer dependencies had the analogous pre-existing ordering
   gap: `Buffer::UpdateCycle` only chained fence completion. It now waits for
   a distinct predecessor's submission before chaining/publishing. Null and
   same-cycle cases do not wait. CPU-dirty staging paths retain their existing
   synchronization.
4. Cancellation now wakes waiters on the cancelled cycle's submission state;
   it does not signal a Vulkan fence or fabricate GPU completion. Cancellation
   while recursively blocked on a different unsubmitted chained predecessor
   remains an inherited limitation; this change does not redesign graph
   cancellation or in-flight GPU cancellation.

The existing queue mutex, full resource barriers, completion chains and binary
semaphore bookkeeping remain in place. No global queue/device idle, semaphore
redesign, per-title conditions, shader hashes or sleeps were added. Independent
review found no introduced deadlock/lifetime defect. Tests cannot prove driver
behavior or exclude every possible dependency graph.

## Host verification before Android build

- `tests/gpu_submission/run.py`:26 behavioral assertions, production
  FenceCycle plus full SubmitInternal/SynchronizeHostInline and production
  buffer methods. Original baseline fails8 affected assertions; buffer-only
  negative control fails2. Fixed code passes UBSan, ASan+UBSan and TSan.
- `tests/gpu_sampled_layout/run.py`:14 byte-exact uploads against an independent
  NVIDIA GOB address oracle, plus mip/pitch/base-layer/explicit-layout controls;
  ASan+UBSan and plain runs pass. Exact production TIC construction and
  swizzle loops execute; Vulkan/Android interfaces are host facades.
- Existing GPU queries15, conditional20, draw-texture16 checks pass.
- Existing pitch suite:310 layouts, production size/stride/copy checks and24
  BC1/BC3 decoder cases pass with ASan+UBSan.
- Broader backing-promotion and framebuffer-cache suites cannot compile in
  this local partial checkout: Boost and Vulkan-Hpp headers are unavailable.
  They are not reported as passing. No device Vulkan test ran locally.

## Performance and device validation

There is no measured Moto G100 FPS/frame-time comparison in this environment.
The submission regressions demonstrate progress before a deliberately
unsignalled GPU fence, for both already-submitted and racing submissions.
The fix removes that unintended completion stall while retaining resource
submission order. Clean/direct buffers may now wait for real predecessor
submission, which can have a CPU scheduling cost. No GPU-completion/global
wait is added. Layout reduction happens only on TIC cache misses and adds no
draws or shader variants. Ordinary independent textures need no additional GPU
copy. Resources that genuinely use different overlapping layouts may require
existing alias readback/rematerialization and submission retry; this cost must
be measured on the device. No per-frame diagnostic readbacks are included.

The new PR workflow retains ccache and enables Gradle build cache, runs both
new host gates before Android assembly, and assembles/uploads only
`assembleDevReldebug` during initial validation. Restore the preferred release
artifact policy separately before any eventual merge if needed.

Use the existing successful PR #356 reldebug artifact as baseline, so no
baseline rebuild is needed. On the same driver, settings and shader cache,
compare repeated menu boots and warmed frame times, then repeat representative
other-game tests (RE5, KOF XIII, SMO, MK8 and another graphics-heavy game).
Check flicker absence, atlas/text fragmentation, stability and frame pacing.
No other-game regression absence or final Naruto cure is claimed without
these runs. No automatic merge.

## Primary/reference sources

- NVIDIA Maxwell sampler override: https://github.com/NVIDIA/open-gpu-doc/blob/9fdf5c4062007929d9f4e6cbad9c9771fe61b880/classes/3d/clb097tex.h
- Texture-unit auto-sizing: https://github.com/envytools/envytools/blob/f102b82381f3f11cee113d16374c87091db039d9/docs/hw/memory/g80-surface.rst
- Eden/Yuzu allocation versus upload: https://github.com/eden-emulator/mirror/blob/de7e6e01d43e8cf2ec193d74fa053b0d7949955f/src/video_core/texture_cache/util.cpp
- deko3d image layout: https://github.com/devkitPro/deko3d/blob/373bfd43d8b2d805fa2eb7d406dbb823032df958/source/dk_image.cpp
- Vulkan sampler restrictions: https://docs.vulkan.org/refpages/latest/refpages/source/VkSamplerCreateInfo.html
- Ryujinx preserved derivative (original unavailable): https://github.com/NextendoNetwork/Ryujinx-Nextendo/blob/2e6ff9ee8bc0072debd6584c3521586d1d4ab5ca/src/Ryujinx.Graphics.Shader/Translation/Transforms/TexturePass.cs
