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
periodic aggregates every 1024 unresolved hits. Physically contiguous recoveries
log only their first 8 cold-path occurrences and every 1024 thereafter. This is not complete split
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

## Jujutsu device log follow-up (Oct 20, 2026)

The new ARM64 Release log reports the repeated exact GPU address 0x407CE0000:
requested 0x1E0000 (1,966,080 bytes), first physical mapping 0x140000
(1,310,720 bytes), two GMMU mappings. No
`Split buffer mapping recovered` line appeared. The counter reached at
least 4096 while warnings were rate limited. This proves the physically
contiguous path in this PR did not solve the observed split request.

The same run has four Turnip VK_ERROR_OUT_OF_POOL_MEMORY reports and ends
with VK_ERROR_DEVICE_LOST at log timestamp 137678469 (about 137.7 s).
The crash is not proven to be caused by this split.

Five `GPU dirty buffer reads for attached buffers are unimplemented`
warnings identify `kepler_compute/pipeline_manager.cpp:21`. There, shader
translation called the ordinary ConstantBuffer::Read<int> instead of
ReadForShaderCompile<int>, despite Dispatch already being protected by
RunWithTextureBackingRetry. The follow-up commit uses the existing
retry-safe read. On CPU-clean reads, no extra Submit/Wait occurs; only
GPU-dirty shader-compile reads unwind to the existing dispatch retry
boundary. This fixes a separate demonstrated correctness hole; whether it
prevents DEVICE_LOST needs Android testing.

Remaining work to fix the noncontiguous split requires a split-only
multi-mapping backing path, preserving the original O(1) BufferManager
and BufferView hot paths and correct GPU writeback/alias/fence semantics.
Do not import #321's global mapping-container rewrite wholesale.

## Follow-up Release log after compute CBUF retry change

Uploaded `emulation.zip` contains Jujutsu v1.0.0 on Turnip Adreno 650
driver 24.0.99. The last message is `VK_ERROR_DEVICE_LOST` at
59,751,045 microseconds, after three `VK_ERROR_OUT_OF_POOL_MEMORY`
messages. The previous `GPU dirty buffer reads for attached buffers are
unimplemented` warnings are absent (zero); the prior bindless tracking
exception remains absent.

There is still **no** `Split buffer mapping recovered` message. Two
disjoint ranges appear in throttled logs:

- GPU 0x407CE0000: requested 0x1E0000, first 0x140000, two pieces.
- GPU 0x40B020000: requested 0x200000, first 0x180000, two pieces.

The warning hit counter reached 3072; this is **not** the total of
logged warning lines, which are throttled. The new Release-log
instrumentation records source-location, the second piece's size and
the rejection reason only on preexisting warned split failures.
It performs no additional work in the contiguous map branch besides
passing the caller's compile-time location to the function. This
diagnostic is not a noncontiguous split fix. Use it to identify the
actual binding category (SSBO, image buffer, vertex/index, etc.)
before implementing proper read/write and fence semantics.

The previous log with five GPU-dirty warnings and device loss at
137.7s also reported Turnip 24.0.99; neither run establishes a
performance regression or causal explanation for DEVICE_LOST
without controlled, repeatable device measurements.

## Confirmed image-buffer caller from the next instrumented device log

The user-supplied Release emulation.log (227 lines) now contains
source locations. **All 20 printed split warnings come from
Textures::GetImageBufferView in gpu/interconnect/common/textures.cpp:540.**
The real call chain is GetImageBufferBinding (common/pipeline.inc),
Textures::GetImageBufferView, CachedMappedBufferView::Update.
It is a Vulkan **storage texel buffer**, not an ordinary SSBO.

Representative observed requests:
- GPU 0x407CE0000: size 0x1E0000; first 0x140000 and
  second 0xA0000. Physical pieces are noncontiguous; sometimes the
  second mapping is absent (unmapped-or-empty-piece).
- GPU 0x41AF50000: size 0x200000; first 0x180000 and
  second 0x80000; observed unmapped-or-empty-piece.
- Unresolved warning hit counter reached 4096. This is a counter of
  *failed cold-path observations*, not a count of printed log lines.
- Zero successful contiguous-split recoveries, zero previous bindless
  tracking exceptions, zero GPU-dirty buffer-read warnings.
- Three VK_ERROR_OUT_OF_POOL_MEMORY driver diagnostic messages;
  GPU loss at 106,581,293 microseconds. Neither the pool messages
  nor the missing mappings are proved to be the *direct* cause of
  VK_ERROR_DEVICE_LOST by this trace alone.

IMPORTANT: The shader fixed in compiler PR #10 uses
`BindlessImageWrite Buffer`, so some affected image-buffer operations
can write. Do not reuse #321's read-only independent aliases for
writable storage-buffer-image descriptors. A partial first-span
VkBufferView silently excludes the second region.

## Specific next implementation boundary

The fix should be on the **split storage-texel-buffer path**, keeping
BufferManager::FindOrCreate(span), Buffer::TryGetView(span) and global
contiguous-buffer lookup unmodified. For real physical discontiguity,
support a coherent linear GPU-visible logical range with per-piece
tracking, preservation of aliased buffer contents, GPU-write
synchronization and host guest-memory commit/writeback to the
correct original pieces. Cache by the exact GPU mapping identity
and invalidate on GMMU changes, guest CPU writes and fence/sequence
transitions. Do not copy every draw or force GPU->CPU readback
on the clean/contiguous path.

Partially unmapped/sparse second pieces cannot be treated as
physically contiguous or written blindly to a null mapping. Inspect
whether mapping becomes valid before relevant GPU writes and handle
sparse semantics separately.

Validation must explicitly cover GPU-written storage texel buffer
spanning two physically disjoint host mappings, guest-visible
writeback, alias coherency, unmapped/sparse second piece, remap,
multiple dispatches, and the device's FPS vs #372 and #374.
Avoid moving #321's global de-dup/sort/lookup into common code.

## Writable split backing follow-up: Sparking! ZERO

The SparkingZERO-Switch-Shipping log (010035F022078000, v2.0.7) has four
recoverable descriptor-pool errors at 12.40, 31.68, 34.13 and 94.59 seconds.
Split image-buffer warnings begin at 34.24 seconds: 0xE0000 requested,
0x78000 + 0x68000 noncontiguous pieces. DEVICE_LOST is at 386.94 seconds.
This chronology does not establish that descriptor-pool exhaustion causes
GPU loss; the log lacks a GPU fault address or final command capture.

The implementation now reuses #321's ordered guest mappings, multi-mirrors,
per-piece traps/dirty tracking and affine delegate reconciliation. It excludes
its independent read-only aliases. Single-span FindOrCreate/TryGetView and
Lookup retain their constant-time containment/page-table returns. Overlap
searches stop at the physical interval boundary instead of scanning all
previous mappings. Sorting/deduplication are restricted to split overlaps.
Exact split views are cached until the existing mapping purge. No gather or
scatter copy is added per draw. Sparse or non-affine/repeated physical layouts
are rejected without binding a misleading first-piece writable descriptor;
these layouts are not fully implemented by this change.

For staged buffers with pending writers or texel readers, backing recreation
unwinds draw/dispatch/DMA/upload preparation, submits preceding work, then
migrates completed source data and rebuilds descriptors. Waiting is limited
to backing recreation that already needs source contents, not ordinary binding.
CPU guest accesses use the existing fence/trap writeback to the original pages.
Texel-buffer views now belong to their Buffer and survive with its fence, so
recycled Vulkan handles cannot reuse a dead backing's view or grow a permanent
Textures-level view cache. This is a source-level lifetime correction, not
proof of the device's final GPU-loss cause.

Essential checks: host syntax check of production mapping/buffer-manager
headers, whitespace and workflow YAML consistency. No tests were added.
ARM64 DevReldebug and runtime/FPS validation remain pending; no Actions monitoring.

Descriptor allocation already accepts OUT_OF_POOL_MEMORY/FRAGMENTED_POOL and
retries with a replacement pool, explaining why execution continues after the
four driver messages. ActiveDescriptorSet ownership is retained by fence
objects; no premature set reuse was demonstrated. One additional cold-path
correction resets an exhausted pool before growth when the allocator is its
sole shared owner. This reclaims descriptors held by idle sets of other layouts
without resetting in-flight sets, scanning retired pools or waiting on the GPU.
Host syntax checks use the pinned Vulkan-Hpp/C headers, a GPU field declaration
shim and Android/Perfetto declaration shims; these are not an Android build.
