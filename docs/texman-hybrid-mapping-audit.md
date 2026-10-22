# TexMan hybrid modernization: physical mapping lookup

Scope: PR #385, branch `feat/texman-ryujinx-overlap-stage1`, extending
`12d584378948f85ccf95d4f75dda43ec2d62da61` without rebasing existing fixes.

## Verified divergences and adaptations

| Mechanism | Reference behavior | Strato change |
| --- | --- | --- |
| Physical overlap lookup | Ryujinx `MultiRangeList` indexes every physical subrange in an augmented interval tree and deduplicates textures. Eden/Yuzu enumerate indexed mapped regions rather than treating the first physical address as the whole texture. | Keep the existing mapping vector and its shared ownership. Add one subtree maximum-end address per mapping; use it for primary and secondary overlap queries, including nested intervals. Preserve descending candidate order. |
| Ordered mapping identity | Ryujinx `FindOrCreateTexture` verifies physical ranges before reusing a matching image, including after mapping changes. | Verify every ordered span's start and end; only the first and last span may be clipped for a view. The old end-only comparison could accept a changed secondary span with the same end, or fail to recognize an exact fragmented match. |
| Subresource byte offset | Physical fragmentation must preserve logical resource offsets. | Sum preceding spans and add the offset within the selected span for mip/layer and 3D slice matching. Do not subtract pointers from different physical segments. |
| Preserving incompatible overlaps | Reference caches distinguish reusable images from overlapping memory history. | On materialization, include primary partial intersections and mismatched mapping candidates at Strato's existing `SynchronizeGuest(false, true)` boundary, as stage 1 already did for secondary spans. Keep retired/replaced filtering and exact alias-group exclusion. |

The old primary lookup binary-searched by mapping end, although insertion of
secondary spans used start-address ordering. Nested interval ends are not
monotonic; even insertion of the primary mapping used this inconsistent order.
All insertion now uses start-address ordering. The index is rebuilt once after
inserting a new texture's mappings, never on an unchanged lookup.

This is an original adaptation of the reference mechanisms, not a transplant of
their memory managers, tracking systems, caches, or texture lifetime models.
Eden/Yuzu's page tables are not copied: Strato already has a mapping vector,
and augmenting it avoids another owner and per-page entry duplication.

## Source snapshots inspected

- [Ryujinx TextureCache.cs](https://github.com/alula/Ryujinx/blob/4930dced25c0d9ec33096b0f2601d262daa477bf/src/Ryujinx.Graphics.Gpu/Image/TextureCache.cs):
  `MemoryUnmappedHandler`, `UpdateMapping`, `FindOrCreateTexture`,
  `UpdatePartiallyMapped`.
- [Ryujinx MultiRangeList.cs](https://github.com/alula/Ryujinx/blob/4930dced25c0d9ec33096b0f2601d262daa477bf/src/Ryujinx.Memory/Range/MultiRangeList.cs):
  per-subrange interval indexing and duplicate elimination.
- [Eden texture_cache.h](https://github.com/eden-emulator/mirror/blob/de7e6e01d43e8cf2ec193d74fa053b0d7949955f/src/video_core/texture_cache/texture_cache.h):
  `ForEachImageInRegion`, `ForEachSparseSegment`, remapped-image rejection,
  registration and deferred deletion.
- [Yuzu texture_cache.h](https://github.com/yuzu-emu-mirror/yuzu-mainline/blob/7ffac53c9e67fa18d067114a8eb3e77c74bba68f/src/video_core/texture_cache/texture_cache.h):
  physical/GPU page-region enumeration, sparse segments and image retirement.

## Cost and synchronization

- Lookup allocates no index storage and prunes subtrees by maximum end and
  minimum start. Common single-span textures retain the same view/backing paths;
  the secondary query returns immediately for them.
- Metadata uses one `uintptr_t` per mapping (8 bytes on ARM64), with vector
  capacity retained across insertions. Rebuilding costs O(N) per newly
  materialized texture; mapping insertion was already O(N). This added CPU work
  needs device measurement, especially in scenes creating many unique textures.
- Existing GPU-written overlap fallback can still download/wait on the source
  texture. Correctly discovering a previously missed overlap can reach that
  existing path more often. No new fence wait, global synchronization, submission
  protocol, image/view rebuilding policy or readback implementation is added.
- No FPS, latency, memory-saving or game-compatibility improvement is claimed
  without Android measurements. Zelda: Link's Awakening and affected UE4 games
  still need comparison on the Moto G100.

## Deliberately deferred

- Automatic invalidation of TIC caches on GMMU remap notifications. This patch
  validates physical mappings supplied to TexMan; it does not intercept cache
  hits that bypass TexMan or install a new GMMU generation protocol.
- Partially unmapped/sparse texture materialization. References have segment
  tracking and upload/writeback policies that Strato's mirror/trap interface
  cannot safely obtain from overlap indexing alone. Invalid spans remain rejected.
- Alias groups for partially overlapping resources, beyond the existing exact
  ordered-mapping protocol. Continued coherence between independently writable
  partial aliases is not solved by this patch.
- Coalescing differently segmented but physically equivalent mapping sequences.
  Matching remains strict to preserve existing alias/trap identity requirements.
- Garbage collection and eviction of retired mappings. Removing strong manager
  ownership requires a separate lifetime/trap/descriptor authority design;
  overlap discovery is not sufficient evidence that destruction is safe.
- The existing 3D slice-source scan remains unchanged.

## Targeted validation

- `ASAN_OPTIONS=detect_leaks=0 python3 tests/gpu_texman_multirange/run.py`
  compiles three small regressions with C++20, warnings as errors, ASan and
  UBSan. Leak detection is disabled only because this execution host prevents
  LeakSanitizer from reading `/proc/.../task`; address/undefined checks stay on.
- `mapping_match.cpp`: exact, clipped, remapped, missing and out-of-order
  physical segments; logical offsets into secondary mappings; single-span control.
- `overlap_index.cpp`: nested intervals, duplicate starts, half-open boundaries,
  empty lookup and 1,600 deterministic comparisons with an exhaustive oracle.
  An access-count assertion checks pruning in a 4,097-entry nested case; this
  is an algorithmic check, not a game performance benchmark.
- `test.cpp`: the original stage-1 secondary-mapping regression, now querying
  the same augmented index as production.
- Existing view-range, sampled-layout and submission regressions are applicable.
  The Vulkan backing-promotion harness compiles the complete production manager;
  its focused lookup case checks fragmented reuse without new allocations or
  submissions, nested mappings, and preservation of a contained GPU writer.
  The host mirror facade supports adjacent split spans; actual disjoint physical
  ordering is covered by the standalone mapping matcher rather than pretending
  that this host facade reproduces Android's mirror VM behavior.

APK builds/workflows and submodule SHAs are unchanged. Full Android compilation
and gameplay are not validated locally. Keep the PR Draft and validate the
existing ARM64 DevReldebug APK on Moto G100 before merging.

### Results and known validation blocker

- Passed: all three standalone regressions under ASan/UBSan; existing
  `texture_view_range_tests.cpp` under UBSan; `gpu_sampled_layout/run.py`;
  `gpu_submission/run.py`.
- Passed: `TEXMAN_LOOKUP_ONLY=1 python3 tests/gpu_backing_promotion/run.py`
  with Clang 18, UBSan, llvmpipe and Vulkan synchronization validation. Both
  image-format-list configurations exercise the complete production
  `FindOrCreate`; zero Vulkan validation errors. Local `CXXFLAGS` mark inherited
  Vulkan/Boost headers as system includes and suppress `-Wreorder-init-list`
  for the existing Clang-supported designated initializer order.
- Negative control: the same lookup case with
  `TEXMAN_BASELINE_MANAGER_REF=12d58437` fails at the contained GPU writer's
  encoded-byte assertion. Current code preserves those bytes.
- **The full legacy backing-promotion/alias suite is not green.** After updating
  its stale host facade/includes and read-only alias expectations to match the
  current production API, it terminates with
  `Cannot losslessly reconcile these texture alias layouts`. Running with
  `TEXMAN_BASELINE_MANAGER_REF=12d58437 TEXMAN_SKIP_LOOKUP=1` reproduces that same
  exception. This broader baseline alias issue remains outside this stage;
  a successful focused case must not be reported as a successful full suite.
- The harness now accepts optional `CXXFLAGS` and restores the matching old
  secondary-overlap header for baseline controls. No CI workflow invokes the
  new tests or requires the host Vulkan case.
