# Stage C — verified prerequisites; mixed authority is not enabled

For the subsequent gated implementation, mapped-entrypoint fixes, validation
and remaining activation blocker, see [region-stage-c-checkpoint.md](region-stage-c-checkpoint.md).

Base: Draft #400 `f474a874753c9fc83e8a6cfb95e11ff2b203a3dc`.
All Stage A/B1a, segment-table and Unmap commits remain in history. No merge,
#421 change, game-specific workaround, sparse/B1b or general rollback change.

## Current increment

This increment does **not** complete Stage C and does not claim automatic
VkBuffer/VkImage coherence. It establishes a real transfer proof and closes a
buffer translation-cache prerequisite before mixed authority can be enabled.

| Case | Before | This increment | Remaining release gate |
|---|---|---|---|
| Cached buffer Update after P→Q, same address/size, no explicit purge | Returns cached P | Translator owner+revision epoch invalidates both single-block and split caches; Q bytes are selected | Bound descriptors not passing through Update still need a common operation resource protocol |
| Same-size constant-buffer selector Refresh after remap | Skips Update | Always reaches stable O(1) Update and epoch enrollment | This is not a fix for any particular game or complete buffer descriptor retranslation |
| Mapping change during buffer preparation | Cache relies on purge; cannot independently enroll cached use | Update enrolls the epoch read before lookup into the existing operation publication lease | Operation-specific producer commit and cached binding preflight remain required |
| Raw block-linear RGBA8 GOB ↔ VkImage | Existing transfers expect CPU-packed linear staging | Tested bounded sector-copy planner, reversible on real Vulkan bytes | No automatic producer discovery/registration calls this geometry helper yet |
| GPU buffer/image shared last writer and resident versions | Image-only regional ledger | Unchanged; mixed registration remains disabled | Typed owner, buffer bindings, selective publication and coalescing migration |
| Partial physical overlaps, CPU callbacks, multiple mixed producers | No verified common authority | No new support claimed | Integration and real-content concurrency/failure tests |
| Sparse, DMI, freed/reused physical allocation | Existing limits | Existing limits preserved | Outside this increment; no translation epoch substitutes for B1b allocation identity |

## Mapping cache semantics

`CachedMappedBufferView::Update` reads `ReadMappingEpoch()` before any block or
split lookup. A different translator **or** revision purges that object's cached
block/view. Every Update, including a cache hit, calls `StartTextureMapping` so
a change during operation preparation is rejected by the existing short
publication lease before new nodes/writers. Explicit Purge still works.

This uses an epoch, not a new per-buffer interval snapshot. Unrelated real map
changes conservatively invalidate the next use of each affected cache object;
unchanged Map/Unmap does not change the translator revision. Stable uses retain
the existing O(1) lookup and do not allocate snapshots or scan either manager.
The epoch adds a shared state reference and scalar per cache. The selector's
Refresh now invokes Update even when the selected size did not change.
Existing upper-bound clamping, contiguous split recovery and explicit unmapped
rejection are preserved. No exception is ignored and no zero buffer is invented.

## Transfer proof

A complete RGBA8 GOB is 64 bytes × 8 rows. Each of its 32 raw 16-byte sectors
maps to 4×1 texels. The planner uses the production layout's Morton sector
coordinates; the test independently computes each byte address.

The planner accepts at most 128 whole GOBs / 4096 rectangles. It rejects invalid
extent/depth, signed coordinate overflow, incomplete edge GOBs, out-of-buffer
ranges, offset overflow/misalignment, duplicate image rectangles and overlapping
raw intervals before returning commands. It has no authority, guest pointer,
format conversion, mapping validation, lifetime or submission side effects.
Callers must prove those contracts separately. Arrays/mips/3D, compressed formats,
sparse holes and arbitrary partial sectors are not admitted by this helper.
Existing Stage A support for those independently supported image classes is not
removed or replaced.

## Why mixed registration remains gated

The code audit identifies concrete integration requirements, not a dependency on
implementing sparse or B1b for still-live dense mappings:

1. `Buffer::SynchronizeGuest` currently copies the whole backing. After a correct
   image handoff this could still overwrite a newer image-owned cell. A mixed
   ledger must mask publication and preserve exclusive buffer bytes.
2. Storage and image-buffer writes call `MarkGpuDirty` during descriptor
   preparation. Publishing typed regional ownership there would leave a false
   writer if a later descriptor forces retry. Buffer writers must be deferred
   and validated alongside image writers before operation publication.
3. `BufferManager::CoalesceBuffers` copies raw bytes, links delegates and then
   invalidates the old Buffer. Typed bindings/residency must migrate before
   retirement; old BufferView delegates are not a lifetime/authority identity.
4. A sector copy must capture actual handles, offsets, versions and old cycles
   while the resources are locked. Resolving a delegate under regional metadata
   would introduce the recreation-mutex/resource lock-order risk.
5. CPU callbacks need nonblocking ownership revalidation and producer
   preparation outside regional/trap locks; the existing fast-readback buffer
   shortcut cannot be used to publish shared authority safely.

Enabling a DMA-only shortcut, removing residence checks, or attaching a buffer
as a texture writer without these gates would be unsafe. The new geometry is
therefore kept separate from activation. The next functional increment is one
bounded dense staged-buffer/image domain with these gates tested together.

## Validation record

See the final delivery for executed commands/results. New negative controls
reproduce cached P bytes after P→Q without purge and fail a deliberately linear
raw-GOB upload on the independent real Vulkan oracle. The cache fix and sector
planner are tested independently: the transport fixture is not presented as
proof of mixed authority, rollback or mixed-consumer lifetime correctness.
No phone FPS, memory/stability measurement or game graphics correction is
claimed. Existing device defects remain the baseline.

## Files and cost

- `interconnect/common/common.{h,cpp}`: cached translator epoch and operation enrollment.
- `interconnect/maxwell_3d/constant_buffers.cpp`: same-size selector Refresh validation.
- `texture/buffer_image_copy.h`: geometry-only bounded raw-sector plan.
- `tests/gpu_cbuf_selector`: exact cache/selector/enrollment source-body regressions.
- `tests/gpu_buffer_image_sequence`: real Vulkan transport/eviction/publication oracle.
- `tests/gpu_backing_promotion`: runs the transport gate in its optional authority probe; configurable sanitizer.
- `tests/gpu_gmmu_identity`: remove an import of deleted diagnostic fixture code.

The cleanup had removed `gmmu_segment_fixture.diagnostics` but left two runner
imports/calls. The resulting ImportError was reproduced and the obsolete imports
were removed; no production telemetry is restored.

No new GPU wait, manager scan, authority participant or allocation occurs on a
stable draw. Buffer cache validation adds shared-state reference operations and
an epoch comparison. A real mapping revision can cause conservative cache
repreparation even for an unrelated interval. Sector plans allocate at most 4096
copy entries per invocation; they are not invoked automatically on draws. Device
CPU time, FPS, memory and driver behavior remain unmeasured.

## Executed host validation (2026-11-04)

Clang 18, lavapipe, Khronos synchronization validation. Driver leak inspection
is disabled (`ASAN_OPTIONS=detect_leaks=0`); ASan/UBSan remain enabled where noted.

| Command / gate | Observed result |
|---|---|
| `python3 tests/gpu_buffer_image_sequence/run.py`, `SANITIZE=address,undefined` | PASS in all four FreeGuest / format-list combinations; final test includes a GPU `copyBuffer` producer, partial overlapping GOB update, independent image producer, guard bytes, actual eviction and production guest publication |
| `BUFFER_IMAGE_LINEAR_NEGATIVE=1` transport probe | Expected FAIL at independent real image-byte comparison; a plain linear copy does not implement the GOB transform |
| Cached buffer epoch negative control, original Update | Expected FAIL: Q request returns cached P bytes without purge |
| `python3 tests/gpu_cbuf_selector/run.py` | PASS under ASan/UBSan; no-purge ABA, stable 1000-use allocation count, translator change, absence/recovery, clamping, same-size Refresh, changed preparation lease/retry and captured MME replay |
| `TEXMAN_AUTHORITY_PROBE=1 python3 tests/gpu_backing_promotion/run.py`, `SANITIZE=address,undefined` | PASS; original A→B→A is **0/1024** stale texels in all FreeGuest / format-list combinations; Stage A layouts, B1a consumers/leases, changed producers, failures, cancellation/retry, CPU callbacks and exclusive bytes remain covered |
| Full `tests/gpu_backing_promotion/run.py` | PASS |
| `TEXMAN_LOCK_ORDER_PROBE=1` | PASS twice, format-list OFF/ON |
| Mapping identity, actual GMMU identity and actual GMMU segment suites | PASS; snapshot bounds/recovery, concurrency, P→Q→P, adjacent Unmap and independent page-byte oracle |
| Region residency, texture sequence, submission, sampled layout, storage preflight | PASS |
| Overlap materialization, framebuffer cache, pushbuffer sequence | PASS |
| Texman multirange, linear readback, draw texture | PASS |
| `git diff --check` | PASS |

The authority sanitizer run preceded the last test-only addition of the explicit
GPU buffer producer; the final standalone transport sanitizer run includes that
addition and the eviction/publication checks. Production code was unchanged
between those runs. No mixed-authority failure/rollback/concurrency result is
claimed: that integration is still absent. Independent review found no blocking
issue and independently ran selector and transport sanitizer tests.

Remote master was also inspected at `a59a5e303cb4bdc49b02da67249db155494f6073`:
its only change after the previously inspected `ab2011a4` is the existing PR
workflow's parallel recursive checkout. No master GPU/shader change is dropped;
this branch does not merge or modify that workflow. All publication uses the
existing Draft #400 and a single synchronization event for ARM64 DevRelease.

## Follow-up CI compatibility (PR Build #41755651787)

GitHub Actions stopped before Android Assemble in the master-provided
`Test conservative Compute SSBO ranges` gate, not in Stage C Vulkan
execution. `tests/ssbo_bounded/run.py` is provided by the PR merge's
**master** side, and its extracted production `CachedMappedBufferView::Update`
body now requires `Gmmu::ReadMappingEpoch`, the cache's `mappingEpoch`,
and `CommandExecutor::StartTextureMapping`. The old host facade lacked
these declarations, so generated `test.cpp` failed to compile.

The separate master-only harness updates
`374dc1c5` and `e6da30c3` add a surrogate identity+revision epoch,
executor hook, and host regressions for P→Q→P without purge and two
translator owners at an identical revision. Production code remains
unchanged; no epoch or cache-validation checks are bypassed. Re-running
the workflow against a new PR merge commit is required to validate the
updated host gate and Android build. This note does not claim the APK
was built or the host test passed in CI.
