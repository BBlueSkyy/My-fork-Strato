# PR #400 continuation: producer-safe partial-overlap materialization

Continuation of Draft #400, preserving its existing five commits, followed first by `9babcf1a` (skip unchanged-layout GPU completion wait). Master remains `9935f133`; no new PR, merge, readback re-port or shader change.

## Concrete correction

The fallback in `TextureManager::FindOrCreate` called `SynchronizeGuest(false, true)` on intersecting textures without the Texture lock that method requires. If the current executor owned a source with a newly prepared GPU write, the source's `cycle` still referred to its previous submission. Waiting on that cycle did not execute the pending producer; the new image could initialize from an old backing.

Before materialization, source writers now carry a `pendingGpuWriteTag` receipt set by the actual `RecordGpuWrite` method. Receipts are cleared when SubmitInternal assigns the new resource cycle, and on unlock (including empty-submit/reset cleanup). A matching executor-owned receipt unwinds through the existing retry handler, which submits queued commands before publishing overlapping sources. Publication holds source resource/state locks. A failed readback at this boundary restores that source's dirty/eviction flags for retry; it does not roll back earlier successfully published sources.

Historical write/readback serial mismatches are deliberately **not** pending-write receipts: alias reconciliation can retain those serials after the guest becomes authoritative. Treating them as pending creates a submit/retry loop. That distinction was found during independent review and covered in the corrected tests.

Lookup/readback order is preserved. Sources are sorted only for lock acquisition. Sorting whole-image readbacks by serial cannot implement physical-region authority, and this change does not attempt it.

## Storage preflight prerequisite reused from #396

The existing mechanism from #396 is selectively integrated in `pipeline.inc` and the graphics full/quick and compute descriptor paths, with new behavioral tests. It is reused, not reimplemented or presented as a new general transaction system.

A writable image resolved early in a descriptor attempt must not claim ownership before a later texture lookup can request this materialization retry. Stage writer pointers in an inline small vector, finish descriptor resolution, allocate DescriptorUpdateInfo, then commit claims. Late resolution or update-info allocation failure leaves no phantom image writer from that attempt.

This does not defer every resource mutation until draw/dispatch publication, protect later descriptor-set allocation failures, or repair retained storage-writer tracking. It is sufficient for this newly introduced lookup retry boundary, not for enabling arbitrary partial-overlap graph retries.

## Evidence and essential checks

- `tests/gpu_overlap_materialization/run.py`: unchanged production materialization helper and executor retry handler, with gated Texture/GPU interfaces. RED using master's old fallback, GREEN after the correction. Checks pending producer ordering, all-source precheck before readback, resource/state lock contention, failed readback/retry, eviction state and no-op/historical-alias cases.
- `tests/gpu_storage_preflight/run.py`: exact production GetImageBinding and the three allocation/commit tails. RED before preflight, GREEN after it. Covers later-descriptor retry, failed allocation, read-only storage and successful commit. Does not run the full pipeline builder.
- `TEXMAN_LOOKUP_ONLY=1 python3 tests/gpu_backing_promotion/run.py`: real Vulkan/lavapipe, UBSan and synchronization validation. A producer clear is recorded but deliberately unsubmitted; actual FindOrCreate rejects the lookup, the actual retry handler submits the producer, and the new overlapping image receives the latest GPU bytes. Free Guest Texture Memory OFF/ON, image-format-list OFF/ON. Eviction/memory/trap interfaces are host facades, not Android MADV_REMOVE. The existing common Texman mapping/reuse controls also pass; zero Vulkan validation errors.
- `tests/gpu_texture_sequence/run.py`, `tests/gpu_submission/run.py`, `tests/gpu_sampled_layout/run.py`: pass. The sampled-layout metadata fixture compiles actual RecordGpuWrite/unlock and verifies receipt acquisition/clearing. Existing copy-reader, failure, layout, alias, mip/layer/format controls remain covered.
- `git diff --check`; `.github/workflows` and build configuration unchanged.

The Vulkan harness required Clang 18 and `-Wno-reorder-init-list` because existing production Vulkan designated initializers are out of declaration order (GCC rejects them). Dependencies were extracted locally; no repository dependency or APK toolchain change.

The broad default backing-promotion suite was also attempted and fails with `Cannot losslessly reconcile these texture alias layouts`. A separate checkout of original #400 `e00e9371` fails identically. This preexisting full-suite failure was not changed or called green; only the focused native materialization/common-lookup Vulkan target above is the gate for this patch.

## #393 oracle retained as a known gap

`TEXMAN_AUTHORITY_PROBE=1 python3 tests/gpu_backing_promotion/run.py` reuses #393's 64x32 native RGBA8 geometry: A and B occupy 8192 bytes each, shifted by 4096 bytes. A writes red, B writes green, then cached A is reused. The required shared region is green.

On this continuation the real Vulkan probe still reports **1024 / 1024 stale intersection texels**. This is a known failing authority oracle, not a passing coherence test. It prevents misrepresenting the new producer-boundary correction as a fix for live A→B→A. No old #393 overlap graph or bitmap footprint construction was imported.

Still pending: persistent physical-region ownership; multiple compatible GPU writers, masked publication and A→B→A partial writes; buffer/texture shared-memory authority; cached sampled/storage descriptor coherence when FindOrCreate is bypassed; complete writer preflight; physical unmap/remap lifetimes; complete Free Guest Texture Memory page/region safety. #392's readback fixes and #395's discard contracts are separate references, not repeated here. No evidence connects the known Sparking! ZERO guest NaNs to this newly corrected boundary.

## Performance and device limits

The first continuation commit removes a demonstrated unnecessary wait for General→General while retaining the pending copy-reader cycle. It does not prove recovery of FighterZ FPS. The producer correction adds no global GPU completion wait, no per-draw texture/cache scan and no image allocation on the common no-overlap path. Its common bookkeeping consists of relaxed receipt stores. Actual overlap materialization allocates small source/lock lists and may now wait for the correct producer instead of reading an old image; overlap-heavy scenes can expose a cost that requires device measurement. GPU-side compatible transfers remain future work.

Test the Release APK with identical driver, resolution, scene and Free Guest Texture Memory setting in FighterZ first; also compare SpongeBob, Sparking! ZERO and Bulletstorm. Keep Draft. No merge, RelDebug, mandatory CI test, Actions monitoring, device FPS or UE4 graphical-fix claim.
