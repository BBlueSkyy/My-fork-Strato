# Fast-path-safe split GPU buffer mapping experiment (stacked on #372)

## Scope and why #321 is not replayed

PR #321 attempted general multi-mapped guest buffers; testing reported an
Android performance regression, and the PR was closed without merge. This
branch DOES NOT import #321. It changes only CachedMappedBufferView::Update,
the corresponding small cache state, and the PR workflow.

Potential regression sites identified from #321 source diff (not independently
measured or proven to be the precise FPS bottlenecks):

- BufferManager::Lookup(span) no longer returned immediately after the
  bufferTable full-range hit; it deduplicated, searched and sorted candidates,
  and acquired their locks on every slow-path lookup.
- BufferManager changed the underlying mapping index to BufferMapping entries
  with extra shared_ptr ownership and extra lookups for split ranges.
- Buffer::TryGetView(span) changed from O(1) span::contains to a linear
  GuestBuffer::Find scan, adding per-mapping work even when splits are absent.
- GuestBuffer::Find, Merge and validation added linear or nested range
  comparisons. These are necessary for some alias layouts but should not be
  on the common contiguous binding path.
- Multi-mirror construction and additional buffer/trap lifecycle tracking
  introduced more work for genuinely split buffers.

None of these costs can be attributed a measured FPS share without device
profiling. The existing BufferManager and Buffer implementations remain
unchanged in this PR.

## Optimization and correctness boundaries

The normal single-block view is unchanged: GMMU LookupBlock plus old
BufferManager::FindOrCreate and view reuse. Additional TranslateRange work
occurs only if the first mapped span is shorter than the exact size and
splitMappingWarn=true.

The cold split path translates all of the requested GPU VA range, verifies
there are no sparse/unmapped pieces, and requires every resulting CPU physical
piece to immediately follow its predecessor. Only in that proven case is a
single physical span passed to the existing BufferManager. This does not
pretend that noncontiguous physical mappings are one BufferView.

An exact (GPU address, requested size) resolution cache avoids repeated
TranslateRange/vector construction for consecutive requests against unchanged
GPU mappings. The existing PurgeCaches invalidation also clears it. Normal
single-block calls are not cached via this secondary path.

For genuinely disjoint mappings, the old first-span behavior is deliberately
retained, with error logging limited to 16 initial global samples and
periodic aggregates every 4096 unresolved hits. This is not complete split
mapping support. In particular, no read-only bounce alias or writable staging
buffer is introduced because the guest/GPU coherence and fence semantics
would require additional work.

Callers that pass an upper-bound size with splitMappingWarn=false continue to
use the old clamping behavior without any TranslateRange.

## Validation and remaining uncertainty

Source review:
- Verify TranslateRange full extent and physical continuity before using
  a single BufferView.
- Verify truncated fallback for noncontiguous, sparse and partially unmapped
  ranges.
- Preserve cache invalidation on PurgeCaches and never assume a write-back.
- Verify no change to BufferManager, Buffer, shaders, descriptors, or bindless
  compiler linkage; #372 remains the functional bindless foundation.

Requires ARM64 DevRelease APK and device measurements. Not yet validated
on device or a local Android SDK/NDK. Tests to compare with #372, on identical
driver/settings and save: Jujutsu boot progress, warning counts, VK_ERROR_DEVICE_LOST,
descriptor pool failures, FPS and CPU/GPU frametimes. Also test Zelda Link's
Awakening and at least one unchanged title for regression.

The final GPU device loss may instead originate from GPU-dirty CBUF reads,
descriptor lifetime/resource use, or another Turnip fault. This experiment
does not claim to fix those separate causes.
