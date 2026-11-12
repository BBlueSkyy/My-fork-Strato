# MK8 pink/white bloom: PR #341 audit and dependency experiment

## Status and scope

Base: master `505bc7d1a572f415032c908989a236c48b1ba533`.
Historical pair: `381d52d359cace0662383adf6622b0f731d12d3d` ->
`3e8689f6c99c4678f473178bd0aa99f8528a615e`. Git confirms the first is
the second's first parent. The complete integration changes 15 files,
2578 additions and 113 deletions; it is not just backing expansion.

**The on-device visual cause remains unproven.** This branch corrects a
reproducible missing dependency in descriptor reuse and supplies the smallest
behavioral A/B candidate identified in this audit. Do not merge on compilation
or host tests alone. No changes to #400, #444 or #445, no full #341 revert,
no shader/gamma/brightness changes and no bloom disabling.

## Evidence

User screenshots from Moto G100 / Snapdragon 870 / Adreno 650:

- Before #341: no large magenta halo at Mario's exhaust or lower-right road,
  and no broad white flare at the right track edge. Separate older black
  texture artifacts are visible and remain outside this investigation.
- After #341: strong magenta halos around the exhaust and across the road,
  broad white flare along the right edge. The track architecture is comparable,
  but other racers and Lakitu differ, so these are not identical-frame images.
  The user's repeated historical APK result is the causal-window evidence;
  screenshots alone cannot distinguish every transient lighting effect.

Inspected the actual third #445 archive, SHA-256
`ac1b5b8bdd08f7efd222e52ed548a8f2f9e0462f94e84765e24acd85d5702e59`:
four VI sources and 12/12 pass readbacks, no failures. The trace orders the
320x180 B10G11R11UfloatPack32 five-mip image, RP37's 1280x720
A2B10G10R10UnormPack32 composite and RP38's 640x360 UNORM preview.
The reported excessive glow is already in RP37, before Android presentation.

The trace contains downsampling/blur at 160x90, 80x45, 40x22 and 20x11,
and upsample FS `05EB6F31815A3BF8` at 40x22, 80x45 and 160x90. Several
inputs and targets share the same five-mip backing. That is legal for
disjoint subresources and does not establish a feedback loop.
FS `F8F8901DE46A8AFF` then samples scene HDR, bloom and auxiliary inputs.
Its attachment blending is disabled. The upsample uses ONE / CONSTANT_COLOR;
the archive does **not** contain effective blend constants, sampler LOD,
exact source/target view ranges, or descriptor-reuse decisions. No blending
fault, wrong mip, stale backing, or corruption is proven by this archive.
Readable magenta content is a clue, not an objective corruption test.

## Full integration audit

| Mechanism | Relevant change and conclusion |
| --- | --- |
| FindOrCreate full match | Adds dimensions/type/stride and actual view-bound validation. May expand a compatible canonical backing. No evidence that MK8's bloom actually invokes expansion; do not assume it does. |
| FindOrCreate mip match | Adds physical per-mip geometry matching and advances mip offsets unconditionally. Before #341, nonzero mip offsets were unreachable because the counters advanced only inside the address-equality branch. This makes real nonzero mip RT reuse reachable and changes which backing receives bloom writes. It preserves a legitimate improvement. |
| View range | Adds requested base offsets to the selected parent mip/layer and validates actual backing bounds. Current views retain range in the image-view cache key. Exact device ranges are absent, so no line-level MK8 mip-selection fault is established. |
| PromoteBackingForView | Uploads CPU-owned old mips, copies existing GPU image mips, initializes only added mips from guest bytes, preserves guest mapping identity and retains old images/views/framebuffers by fence. Increments backingGeneration and resets download storage. No direct data-loss defect identified in the audited code. |
| Descriptor/view lifetime | Backing promotion submits prior work through a retry boundary; submission invalidates graphics descriptor state. TextureView::GetView checks backingGeneration before returning cached native views. No demonstrated stale-image descriptor caused by promotion. |
| Exact aliases | Introduces a single writer/generation group, stale alias rejection, guest-bridge reconciliation and cached-view coherence checks. This is distinct from several mip views of the same Texture. No evidence that the bloom backing needs separate alias reconciliation in the captured interval. |
| Transfers and layouts | Adds image-scoped read/write -> transfer-write -> subsequent read/write dependencies on reuploads and all-writer -> transfer-read dependency for downloads. Promotion retains dependencies and resources. No justification found for reverting these barriers. |
| Ownership/traps | Sets up weak trap callbacks after shared ownership exists, tracks actual RT/storage writer serials, prevents stale/read-only aliases from overwriting guest memory and leases guest bytes for growth. No proven connection to the reported bloom in the available capture. |
| Render pass/descriptor reuse | Shared mip backings expose an existing gap: PopulateReadBarrier is called from GetTextureBinding, which descriptor reuse bypasses. An unchanged descriptor can refer to an image written since its last resolution. This is independently reproduced by the new test. |
| Presentation | #341 reconciles retained display aliases. RP37 evidence places the symptom earlier, so the Android presentation path is not the first occurrence. |

Later master changes strengthen mapping matching, tile-spacing handling and
alias routing. They are retained. The historical comparison is the exact
parent/integration diff; the experiment is applied to current master.

## Proven dependency defect and candidate correction

1. A full descriptor resolution calls `GetTextureBinding`, which calls
   `Texture::PopulateReadBarrier` for its actual shader stage.
2. `Texture::UpdateRenderPassUsage(RenderTarget)` marks shader stages pending.
3. `Maxwell3D::PrepareDraw` can reuse an identical descriptor set, or partially
   update only a constant buffer that does not hold its image handles.
4. In both cases, some sampled views bypass `GetTextureBinding` entirely.
   Their RT-write -> shader-read stage masks are omitted. Reusing descriptor
   bytes is not evidence that the underlying image has no new writes.
5. `Pipeline::PopulateSampledImageReadBarriers` revisits the already resolved
   views using their stage-specific packed descriptor counts **after** partial
   updates. It uses the existing pending/read-stage tracker and memory-barrier
   emission. It neither changes mip ranges nor forces descriptor rewrites.

Full updates keep their original path. Repeated visits after descriptor
resolution are idempotent because pending stage bits are consumed once.
Null descriptors and inactive stages do not create accesses. This also restores
read-stage tracking needed by later sampled -> RT dependencies.

The omission predates #341. **The hypothesis**, not a proven device fact, is
that #341's newly reachable mip sharing exposes it in MK8's cached bloom
bindings. Shader LOD/view-range and actual blend constants remain competing
explanations if the A/B does not change the visual result.

## Implementation/validation plan and evidence

- [x] Audit the complete parent/integration diff and original #445 archive.
- [x] Isolate the missing dependency with production descriptor selection and
  texture tracker bodies; require failure against unchanged master.
- [x] Add the minimal per-stage refresh only on descriptor-reuse draws.
- [x] Run ASan/UBSan positive and negative controls and mapping/layout suites.
- [ ] Complete ARM64 DevRelease CI and publish its artifact.
- [ ] Validate matching settings on Moto G100; only then judge MK8 causality.

`python3 tests/gpu_sampled_barriers/run.py` compiles the exact descriptor
decision region, new pipeline traversal and production texture read/write
tracking. Host facades supply descriptor resolution and stage metadata; this
does **not** execute Vulkan draws or prove mobile visibility/performance.
`SAMPLED_BARRIERS_BASELINE_REF=505bc7d1` must fail with
`cached sampled descriptor lost color-write source dependency`.
The fixed code passes unchanged reuse, uniform-only quick bind, partial image
updates, full updates, compatible pipeline reuse, repeated reads, null/array
elements, depth writers and replacement-before-refresh ordering.

Also run `tests/gpu_sampled_layout/run.py` and
`ASAN_OPTIONS=detect_leaks=0 python3 tests/gpu_texman_multirange/run.py`.
The backing-promotion Vulkan suite compiled, then aborted with
`Cannot losslessly reconcile these texture alias layouts`, identically on
this branch and a separate clean worktree at `505bc7d1`. This is a reproduced
baseline failure, **not** a passing Vulkan suite. It is outside this patch;
none of the manager, texture, alias or promotion bodies extracted by that
suite differ between these two trees. No all-green GPU-suite claim is made.

## Minimum device experiment and remaining risks

Compare this branch's ARM64 DevRelease with master `505bc7d1` using the same
MK8 update, driver (captured Turnip 24.0.99), save/grid, resolution and settings.
Start with Free Guest Texture Memory **OFF**, cold boot each, and record the
grid plus a short race. Repeat to separate race timing from the stable halo.
Test ON independently afterward. No capture flag is needed for this behavioral
A/B. Do not compare armed-capture FPS with ordinary gameplay FPS.

If the excessive pink/white bloom persists unchanged, this missing dependency
was not a sufficient explanation. Do not stack speculative brightness fixes.
The next minimum evidence is the exact cached-vs-resolved decision, source/RT
VkImageSubresourceRange, sampler LOD and effective blend constants on the
identified bloom draws; obtain those in an independent diagnostic branch,
without editing #445 or repeating broad readbacks.

Risk: a linear scan of sampled elements on reused draws and dependencies only
when the existing tracker has pending writes. No allocation, fence wait, upload,
image replacement or descriptor write is added. Added synchronization may expose
performance cost; no device FPS/frametime measurement exists yet. Whole-texture
tracking remains conservative across disjoint mips; this patch does not invent
subresource ownership or prove same-subresource feedback.

Required phone checks: MK8 (grid, bloom, menu textures, race stability), SMO
(bloom/fog and scene transitions), BOTW (lighting plus a short traversal), RE5
(cubemaps/black flashes and gameplay). Separate preexisting failures from
changes against this branch's master base. #444 is not integrated here, so
its separate MK8-menu/SMO-fog corrections are not acceptance claims for this APK.
