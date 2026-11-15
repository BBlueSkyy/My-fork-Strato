# Session C — HDR image-only regional authority capability (2026-11-15)

## Decision

**Outcome C for integration of the actual PR453 HDR component.** The existing
image-only regional ledger is reusable and already transports packed HDR bits
correctly in a bounded full-GOB control. It cannot admit the footprints needed by
the recorded component. No renderer extension, guard relaxation, new runtime
option, mixed activation or APK is justified by these results.

This is a capability investigation, not a fix for the MK8 glow. The deliberately
strict actual-footprint coherence gate remains RED. A green control fixture is
not evidence that the Android frame is correct, nor that its performance is safe.

## Checkpoints and evidence

Remote heads were checked before changes and matched the requested checkpoints:
PR400 `a448ad06e84f0edd193520c08d17e5d1b819e56f`, PR453
`88cc2d0d6f35da58da2dc3b9626519dc6f271038`. Work is isolated in PR400's existing
branch. PR453 is read-only. Seven published predecessor checkpoints were retained
as local refs (a448ad06, e2c88f2d, ee982927, 5c16bec9, 2eefb906, bd93f7b9,
3190dbfc). The exact unpublished `fdebbe431f4bb3824035ed869a90168babbf3e1b`
mentioned in B2 is absent from all available object stores; its published successor
and history were retained. It was not fabricated or silently discarded.

The complete PR453 audit, events.json, events.txt and
`CheckLoggedPartialHistories` were read at the immutable reference above:
[PR453 audit](https://github.com/BBlueSkyy/My-fork-Strato/blob/88cc2d0d6f35da58da2dc3b9626519dc6f271038/docs/pr453-partial-overlap-audit-20261115.md).
Its inventory records 40 events, 1 PREFIX ACCEPT, 1 DONOR ACCEPT and 10 DONOR
REJECT; no promotion. The two partial rejects precede the accepted complete donor.
Nothing here reclassifies those rejects as harmless/obsolete representations.

The native fixture relocates the recorded physical allocations into ordinary
host memory. With B = the recorded 640x360 child start:

| Representation | Start relative to B | Allocation bytes |
| --- | ---: | ---: |
| child, 1 mip | 0 | 983040 |
| requested parent, 2 mips | 0 | 1310720 |
| complete mip1 donor | 983040 | 327680 |
| partial, 5 mips | 1048576 | 438272 |
| partial, 1 mip | 1048576 | 327680 |
| four smaller histories | 0 | 2048 / 6144 / 20480 / 81920 |
| larger history | -524288 | 1966080 |
| two larger histories | -1048576 | 3932160 each |
| enclosing history | -13303808 | 16777216 |

Each partial begins 65536 bytes into the donor. It shares 262144 bytes with mip1
and has respectively 176128 or 65536 exterior bytes. A whole-mip image alias or a
one-time copy cannot retain both future readers and those exterior histories.

The principal 640x360 layouts, 320x180 donor, BH16/BD1/spacing0 and their sizes are
confirmed. The secondary TICs are incomplete: the fixture uses explicit synthetic
HDR shapes matching their allocation sizes. In particular its five-mip 320x180
and one-mip 320x256 shapes, and the enclosing images' HDR formats/dimensions, are
hypotheses, not recovered Android TICs. Exact interval reproduction is not exact
secondary-layout reproduction. Recorded `format_or_image_flags` rejects do not
identify the actual alternate formats.

## What can already be reused

- `RegionAuthority`, `PhysicalImageRegion` and `PhysicalImageBinding` distinguish
  current owner/host version, guest version and each image's resident version per
  physical cell. Their weak writer references do not destroy live retained views.
- `RegisterRegionAuthority` is the **image-only** path and is independent of
  `MixedRegionAuthorityEnabled`. Same native optimal Vulkan formats, reversible
  layouts, subresource coordinates and complete span geometry still have to fit.
  There is no RGBA8-only whitelist in this image-only path. Merely adding HDR to
  the mixed whitelist would address the wrong restriction.
- `RecordGpuWrite`/`RecordRegionWrite` and `InvalidateRegionPeers` maintain future
  ownership and stale peers across frames. `regionInvalidationSequence` supplies
  descriptor reuse invalidation; unchanged native image/view identities need no
  replacement descriptor solely for a content refresh.
- `ReconcileRegions` uses the recorded source/destination mip/layer/rectangle,
  copies only stale foreign-owned cells, sequences prior cycles and retains
  native owners. Resident versions are committed after successful submission.
  The no-op fast path does not reconstruct or globally search the component.
- `PublishRegionGuest` masks publication by current owner and advances only those
  guest versions. CPU callbacks first publish relevant owners; a store then
  invalidates later resident reads rather than discarding dependency tracking.
- `GuestDependencyDomain`/`RegionLink` preserve future dependencies after mixed
  withdrawal. They solve dependency lifetime, not a missing HDR edge mapping:
  their supported guest transport, preparation and admission proofs remain bound
  to the previously validated layouts. They are not an HDR escape hatch.
- `SynchronizeGuestOverlaps` rejects an executor-tagged unsubmitted producer
  before touching guest bytes. Reading its previous fence would otherwise encode
  old pixels. It is a publication bridge, not a substitute for persistent partial
  authority after later writes.

`gpuWriteSerial`/`gpuReadbackSerial` are whole-resource receipts, not per-mip
history. `backingGeneration` identifies native storage changes, not which physical
bytes are newest. Serial zero can mean valid CPU-uploaded content. Image-only
registration preserves inherited regional versions, but a newly untracked group
starts from a guest baseline; it cannot infer independent past mip/partial owners
from a single resource serial. The positive control registers before GPU writes.
Late untracked histories need a proved publication/bootstrap transaction.

## Concrete boundaries preventing the actual component

1. `PhysicalImageSpans(..., false)` requires dense reversible rows without
   allocation padding (`blockLinearSize == linearSize`) and complete block-height
   rows. The padded 640x360 allocation fails that dense proof.
2. Full-GOB mode admits 640x360 mip0: 40 columns * 45 GOB rows = 1800 full
   GOBs. However 320x180 mip1 has 180 % 8 = 4. Its last GOB row is only partly
   logical texels; its physical allocation also contains block-height padding.
   The current `rows % 8` guard rejects the parent and donor (and the selected
   five-mip control's edge levels). Removing it would publish/copy padding as
   though it were native pixels. Padding in one layout can be valid pixels in
   another, so allocation intersection alone proves nothing about these bytes.
3. Different layouts/dimensions/mip boundaries cause registration to fall back
   from dense spans to a common full-GOB partition. The selected 2048x2048
   enclosing image has a dense span alone but needs 32768 GOBs in that partition,
   above the existing 4096 per-image budget. The 1280x720 synthetic enclosing
   shape also exceeds that budget. Raising it is not a correctness/performance
   proof; the positive control already has substantial metadata.
4. A cell needs compatible source/destination native rectangles (`rowBytes`,
   `rowHeight`, width, mip, layer), not merely equal addresses. Arbitrary sector
   cuts, different actual formats and incompletely observed TICs remain unproved.
5. `RecordRegionWrite` claims **all bindings**, even for a mip0-only operation.
   In the image-only path it first reconciles the full image. This can safely
   transfer ownership of unchanged, freshly prepared data: the new test verifies
   preservation of a foreign latest mip1 and exterior content. It is not an
   exact written-texel receipt and must not be used to seed unprepared padding or
   late independent histories. Executor preflight/commit separates allocating
   preparation from publication; bypassing that contract is not supported.
6. The mixed Buffer/Image raw path remains restricted (RGBA8 variants, bounded
   staged/reversible layouts, one mip/layer in its supported transport). The B2
   dependency domain does not independently relax that transport. No format-only
   expansion, Android activation or PR453 consolidation was implemented.

## Tests and observed content

`tests/gpu_hdr_region_sequence` reuses the extracted production methods from the
real native backing harness, with independent block-linear byte addressing.
Images, copies, command buffers, views and fences are native Vulkan. Allocator,
scheduler interfaces and trap dispatch are host facades. Validation includes
synchronization validation on lavapipe, not Turnip/Android.

**Negative gate:** after conservative registration refusal, successive native
writes through parent, donor and partial mip0 leave retained parent and donor
readers stale: 212543 and 152410 logical bytes in the deterministic patterns.
The ordinary test asserts this counterexample; `--require-actual-coherence`
intentionally exits nonzero with `HDR_REGION_STALE_READER`. Zero Vulkan validation
errors precede that content failure. Rejected registration creates no native image
or published regional link. The separate native encode/readback test checks valid
HDR pixel bits and preserves all bytes outside that layout's logical-pixel mask.

**Positive control:** six full-GOB HDR images use 512x256 prefix/2-mip parent,
256x128 donor/partial-one/partial-five and a 512x512 enclosing representation.
These different dimensions are intentional: this is the model's supported boundary,
not an assertion that MK8 fits it. Normal and reverse creation orders both pass:

- Three rounds of alternating native GPU writers with retained views; compare
  every logical pixel against the physical oracle after each group of writes.
- A mip0-only parent write preserves the latest foreign mip1 and exterior bytes.
- Raw packed HDR encodings (finite, subnormal, infinity and NaN patterns) survive
  native copies and readback bit-for-bit on this ICD. No shader arithmetic is used.
- Command-buffer allocation/preparation, owner attachment and submission failures
  leave stale state/prior cycle valid; retry repairs the complete pixel content.
  The facade flag named `failRecording` throws before allocation; native Vulkan
  begin/copy/end recording failures are not separately injected.
- A real command buffer holding a copy is still unsubmitted when the actual
  overlap bridge is called with its writer tag. The typed request leaves guest
  bytes and submission count unchanged. After submission, publication and future
  regional reads match the byte oracle.
- CPU publication through the connected overlap callbacks, a CPU store, completed
  readbacks, exterior/guard bytes, and destruction/recreation preserve content.
  Native image/view handles stay unchanged during ordinary repair. Content is
  verified by backing readback, not a shader sampling those HDR view descriptors;
  descriptor behavior is separately covered by the existing general regressions.

An initial CPU test failed by one byte because it invoked only one selected
image's trap callback. The same failure reproduced in the RGBA8 control. The real
TrapManager visits overlapping/recursive entries; the test now dispatches the
connected callbacks before storing. No production behavior or content assertion
was weakened to fix this test setup.

The full native regression separately covers real backing promotion, pending
old backings, alias/cache/view bounds, lifetime and failure recovery. The new HDR
fixture does not consolidate or replace HDR backings, prove concurrent admission
with unknown TICs, or repair the actual-footprint negative gate.

## Measured host costs

The aligned control's 1 MiB physical union uses 2048 cells and 5205 bindings:
163840 + 374760 = **538600 bytes** of struct storage, excluding vector capacity,
weak-member lists, allocator overhead and image memory. A naive full-GOB
partition for larger enclosing resources is not free.

Across three scripted rounds (15 explicit producer uploads and 36 oracle
readbacks), both creation orders record 0 new images, 51 staging allocations,
87 submissions and 36 invalidation epochs. Of those submissions, 51 are the
fixture's explicit uploads/readbacks and the other 36 are coherence repair.
No copy-region count, bandwidth estimate or Android cost is inferred from that
submission count. This is not a before/after renderer benchmark.

For 600 already-coherent preflights: 0 host `operator new` calls on the tested
thread, 0 native image/staging allocations, 0 submissions, 0 new invalidations.
This verifies the bounded stable path only. It does not measure Moto G100 FPS,
frametime, peak memory or concurrent CPU threads.

## Commands, regression results and limitations

Native prerequisites/host flags are recorded in the evidence directory. Run:

```sh
python3 tests/gpu_hdr_region_sequence/run.py
SANITIZE=address,undefined python3 tests/gpu_hdr_region_sequence/run.py
python3 tests/gpu_hdr_region_sequence/run.py --require-actual-coherence
python3 tests/gpu_backing_promotion/run.py
TEXMAN_AUTHORITY_PROBE=1 python3 tests/gpu_backing_promotion/run.py
TEXMAN_MIXED_WITHDRAWAL_BOUNDARY=1 SANITIZE=address,undefined python3 tests/gpu_backing_promotion/run.py
python3 tests/gpu_buffer_image_sequence/run.py --automatic
```

The strict third command is expected RED; the others pass. Automatic mixed tests
use the existing isolated test-header copy, never the production gate. Native
results run both format-list modes. ASan uses `detect_leaks=0` because this managed
runtime cannot use LSan's process tracing; this is not a leak-free claim.

Related passes: texture sequence, regional executor/residency, mapping identity,
GMMU identity/segments, pushbuffer sequence, Texman multirange, sampled layout,
submissions, framebuffer cache, draw texture, storage preflight and overlap
materialization. The first framebuffer invocation failed on external Vulkan-Hpp
clang19 deprecated-copy diagnostics: it passed with the existing host compiler
wrapper. No framebuffer assertions/source were changed.

The extra conditional-rendering regression **does not compile at the baseline**:
its facade lacks the existing Buffer region-preparation API, descriptor access
lists and index invalidation/storage-image members required by extracted source.
All its inputs are byte-identical to a448ad06. It was not weakened or claimed
passing; this unrelated harness repair is deferred. Therefore this investigation
also does not claim every repository suite green.

The native baseline initially failed on three ambiguous `vk::Extent3D` assignments
with the installed Vulkan-Hpp; explicit types restore compilation without changing
values/assertions. These test-only repairs were verified before the HDR additions.
No production source, workflow, shader, gate or PR453 file changes in this session.

## Required next proof

A narrow follow-up should first implement a **test-only reversible edge-cell
mapper**: distinguish live texel sectors from padding, intersect two layouts and
prove bit-exact clipped source/destination rectangles across mip edges and exterior
bytes. Include incompatible format/geometry refusal. Then prove a bounded adaptive
partition for the enclosing allocations without flattening all of them into GOBs,
and a late-registration transaction that preserves each already-live writer and
future reader. These are admission/transport prerequisites, not a request for more
broad Android instrumentation. Existing version/publication/invalidation types can
be reused only once those proofs hold.

The complete recorded secondary TICs remain a limitation for an exact MK8 layout
claim; no fresh device logs or capture system are added here. Production integration
and APK remain blocked by the unrepaired content gate. PR400 stays Draft; PR453
and all existing renderer behavior stay unchanged. No merge.

## Final source review

A fresh read-only review reran the native HDR fixture successfully and found no
Critical/Important issue in publishing this bounded tests/evidence change. Its
minor failure-stage wording finding was corrected: `failRecording` is allocation/
preparation injection, not a native Vulkan recording error. No assertions changed.

The review did not certify Android correctness/performance, unknown secondary
TICs, reversible edge transport, late writer bootstrap, general mixed HDR, HDR
consolidation/pending backing replacement, leak freedom or the baseline conditional
facade. These remain explicit limitations, not omitted release gates. Existing
broader regression evidence was inspected; only the HDR suite was rerun by the
reviewer. No new runtime behavior or build is being approved.
