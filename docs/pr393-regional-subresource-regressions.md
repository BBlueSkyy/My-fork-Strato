# PR #393: regional-authority regression investigation

Checkpoint: `95ea4d7951b3ff9700b33130caf3b28ec337d0bb`, master `e8e38237aa5993deeaaae9f01f39ea08500f18db`.

## Device evidence and limits

The supplied SMO log records `Concurrent writes through partially overlapping textures cannot be ordered` from ValidateAliasWrites -> CreateRenderPassWithSubpass -> Draw. It identifies Turnip Adreno 650 / Vulkan 1.3.280, but **does not record the two resources, their mip/layer views or physical footprints**. Therefore the log proves the throwing path, not whether this particular pair has a true or false write conflict. MK8's red screen/approximately 2 FPS and SpongeBob's green/excessively bright output are user observations, not resource-level traces.

## Checkpoint boundaries identified before changing code

At the checkpoint, the ledger records physical texel intervals, but every write claims the complete Texture footprint. ValidateAliasWrites likewise compares complete Texture footprints, ignoring TextureView::range. One stale flag covers a whole representation; reconciliation reads all pending owners needed for that representation and uploads its complete contents. Thus the current integration cannot isolate independent mip/layer writes or avoid a full reconstruction when only one part became stale.

A complete Ryujinx-style partial-transfer/dependency framework would be a larger redesign. Do not transplant it here. In particular, this update will not establish exact raster/scissor coverage, color/depth/stencil bit masks, partial 3D depth slices, GPU-to-GPU region copies, lossy format conversion or a new memory/GC architecture. True overlapping writes through distinct host images in one operation remain rejected. Rectangular writes remain conservatively scoped to their selected mip/layers.

## Primary-source comparison

Reference: Leuconoe/Ryubing commit `69f2c0942452637b7be6965efea390f1868b8d9c`, `src/Ryujinx.Graphics.Gpu/Image/` (local pinned source, not a claim about latest upstream).

| Reference | Semantics | #393 gap |
| --- | --- | --- |
| TextureGroup.EvaluateRelevantHandles / SignalModifying | Select relevant level/layer handles, with explicit 3D handling | RecordGpuWrite discards the selected view range |
| TextureGroup.SynchronizeMemory / SynchronizePartial | Evaluate modified/dirty/deferred-copy handles; load only selected handles when partial synchronization is needed | ReconcileOverlaps uses a whole-representation CPU bridge |
| TextureGroupHandle.SignalModified / DeferCopy / CreateCopyDependency | Modified state and deferred copies belong to relevant handles, retaining source ordering | #393 has one owner ledger plus coarse representation staleness |
| TextureDependency.SignalModified | Notify the paired handle to defer a copy from the modified handle | #393 marks an entire overlapping representation stale |

No reference code is copied. A bounded correction may pass the actual view footprint into validation and write claims, and use existing ledger serials plus a successful full-upload snapshot to distinguish an independently current view from a stale one. Necessary reconstructions remain whole-image and must preserve exclusive GPU-only data. That is a limit, not an equivalent implementation of Ryujinx's dependency system.

## Validation requirements

Before an APK: reproduce the false-conflict/overclaim cases with independent views, prove real conflicts remain rejected, check actual Vulkan bytes for exclusive GPU data, A->B->A, cached views/quick bind, Free Guest OFF/ON, failed transfer recovery, mip promotion, lifetime, ASan/UBSan and synchronization validation. Measure deterministic readback/upload/submission counts for stable independent reuse; do not claim Android FPS recovery from host tests.


## Bounded correction

- Intersect the existing encoded-texel footprint with the selected mip/layer range, using logical mirror offsets rather than subtracting disjoint physical addresses. Cache these selections on the existing OverlapState; clear them after successful mip growth. No new texture identity or authority framework.
- Validate the actual selected byte sets before publishing a render-pass node. Regional exact peers use this same physical-byte guard; exact-only groups retain their previous policy. Read-only/retired writers and genuine overlapping writes through distinct host images remain rejected.
- Pass the view range through RT, storage-image, draw-texture and 2D blit writes. Writes claim only those selected texels; the generic UsageTracker still conservatively tracks complete physical mappings.
- A write to one subresource does not clear the stale flag for unrelated subresources. A current view can consult the ledger and continue using its image even if another part of that representation is stale.
- Before a full upload, inspect all ledger intervals in that representation. Only if their guest bytes are already valid may the current GPU-write serial be captured; commit this snapshot after successful upload. Otherwise the upload leaves the snapshot invalid. A later CPU publication cannot retroactively validate bytes copied earlier. A foreign interval newer than a valid snapshot (or with unpreserved guest data) remains a dependency. Failed uploads cannot advance or change the previous snapshot. Whole-image reconstruction remains the fallback when needed, preserving each GPU-only owner's bytes.
- Cached descriptor/quick-bind/RT guards pass the actual view range. AttachTexture registers acquired-lock ownership, then validates the view under the resource lock, including after waiting, before any command node. GetView retains its view-aware guard; remove the earlier manager check that used the whole backing before GetView selected its range.

The existing globalChannelLock serializes GPU-write publication; this correction introduces no new cross-channel locking protocol. The snapshot is not a GPU completion fence. Existing cycles, submission ordering, pending receipts, traps, Free Guest component policy, sparse handling and texel-buffer semantics remain in place.

## Reproduction and validation

| Check | Checkpoint / negative control | Corrected tree |
| --- | --- | --- |
| Independent selected RT layers, partially overlapping backings | ValidateAliasWrites rejects with the same error class as the SMO log | Complete production CreateRenderPassWithSubpass accepts the independent views |
| Actual selected bytes overlap | Rejected | Still rejected before publishing nodes or advancing writer serials, including regional exact peers |
| Write selected layer; inspect unselected layer ownership | Original RecordGpuWrite from 95ea4d79 claims the untouched layer (test assertion fails) | Untouched mip/layer ownership remains unchanged |
| Eight mip/layer × contiguous/fragmented × Free Guest OFF/ON cases | Old write body fails the authority oracle | 32 alternating independent writes per case produce exactly 32 write submissions, zero bridge readbacks/uploads and zero new image/staging allocations |
| Exclusive GPU-only data and necessary reconstruction | Deterministic byte oracle | Red A-only and green/blue shared data survive; cached independent views avoid retries; stale selected views still request reconciliation |
| Failure recovery | Snapshot must not advance before a successful upload | Allocation failure retains the prior snapshot, then retry succeeds |
| CPU store, new foreign GPU write, selected-view upload, then foreign publication | An end-of-upload global serial incorrectly accepts the stale sibling (the new test fails before the snapshot fix) | Capture validity before reading guest bytes; stale sibling still requests reconciliation and final image preserves CPU and GPU bytes |

The original false-conflict reproduction failed before the correction. During validation, the new CPU/GPU interleaving exposed a flaw in the initial per-image snapshot correction: global serial freshness alone did not prove the staging source was valid. The fixture uses the real page trap, an actual CPU store, a subsequent foreign GPU write, and publication only after the selected-view upload. It fails at the stale-view guard before the validity check, and passes with exact final-byte verification after it.

The separate legacy-write control substitutes only the original RecordGpuWrite body into the host fixture; it fails at the untouched-region owner assertion. It is not an APK toggle or runtime instrumentation.

Passed with real lavapipe and zero Vulkan synchronization-validation errors:

- Full regional suite with ASan/UBSan (re-run after the snapshot correction): original 24 trap/reuse/pending/Free Guest permutations, A->B->A, padding, late aliases, retained descriptors, failed transfers, contention, mip growth, plus the new independent-subresource cases.
- The eight focused view cases also pass with fragmented physical order deliberately reversed relative to logical mip/layer order.
- Production backing/alias/promotion suite under UBSan, including pending receipts, lifetime, Free Guest OFF/ON, historical writers and 1-to-8 mip growth. The test RT writer now passes the actual view range, matching production.
- Submission/FenceCycle lifetime/ordering, sampled-layout/alias and production encoders, and draw-texture geometry/lifetime checks. Host interfaces were adapted for the optional range argument; production guards are not replaced by no-ops in the regional fixture.
- 1,186 TIC checks and 130 cache/descriptor checks, including Vulkan lifetime with ten pipeline variants retained behind pending fences (zero validation errors).

ASan/UBSan remain enabled; LeakSanitizer is disabled for this host environment. The new content tests issue real Vulkan subresource clears and inspect images/guest bytes. Full render-pass preparation uses the production method with pipeline/node facades; it is not a complete guest shader draw.

## What remains unproven / deferred

The supplied log lacks mip/layer/footprint identities. The corrected false-conflict class and overclaim are proven host defects, but their contribution to the exact SMO crash, MK8's red screen/2 FPS or SpongeBob's colors cannot be established from that log. Device stability, graphics and FPS must be retested.

Before a component is registered, historical authority remains conservative for an entire current representation; existing regional components retain their interval ledger when merged.

There is still one dirty state and a whole-image transfer path. Truly stale subresources may cause full-image readback/upload and can remain expensive. Views of a 3D backing still use conservative full footprints; rectangular/scissor/color/depth/stencil write coverage is not represented. Storage-write versus RT hazards within a single operation are not replaced by a new general dependency system. No protections were removed to force these unsupported cases through.

Footprint selections share the Texture's existing lifetime and have no new eviction policy. No cache/GC rewrite, reference implementation transplant, new format handling, game-specific workaround or new gameplay logging/validation is introduced. Master shaders, submodules, traps, kernel memory and sparse/texel implementations are preserved. Build ARM64 DevRelease only after these validations, keep the existing PR Draft and do not merge; do not monitor Actions, per the user's preference.

Primary references: [TextureGroup](https://github.com/Leuconoe/Ryubing/blob/69f2c0942452637b7be6965efea390f1868b8d9c/src/Ryujinx.Graphics.Gpu/Image/TextureGroup.cs), [TextureGroupHandle](https://github.com/Leuconoe/Ryubing/blob/69f2c0942452637b7be6965efea390f1868b8d9c/src/Ryujinx.Graphics.Gpu/Image/TextureGroupHandle.cs), [TextureDependency](https://github.com/Leuconoe/Ryubing/blob/69f2c0942452637b7be6965efea390f1868b8d9c/src/Ryujinx.Graphics.Gpu/Image/TextureDependency.cs).

Input SMO log SHA-256: `f11a213c9f282bee5db3d5952e38b0a415db8d57c385d8c958e877dd512ace0f`.


Reproduction commands (with the host Vulkan/toolchain dependencies installed):

```sh
ASAN_OPTIONS=detect_leaks=0 CXXFLAGS="-Wno-error=deprecated-declarations -fsanitize=address,undefined" python3 tests/gpu_overlap_authority/run.py --check-correct
CXXFLAGS=-Wno-error=deprecated-declarations python3 tests/gpu_backing_promotion/run.py
python3 tests/gpu_submission/run.py
```

`TEXMAN_SUBRESOURCES_ONLY=1` limits the regional executable to the new view/snapshot cases. `TEXMAN_LEGACY_WRITE_REF=95ea4d79` is the failing historical write-claim control; it is confined to the host runner.
