# PR400 HDR regional capability investigation plan

Goal: prove the current image-only authority model against PR453's physical HDR
intervals, without enabling MixedRegionAuthorityEnabled or modifying PR453.

Start: PR400 a448ad06e84f0edd193520c08d17e5d1b819e56f; PR453
88cc2d0d6f35da58da2dc3b9626519dc6f271038. Both remote heads confirmed unchanged.
Seven published predecessor checkpoints preserved locally. The unpublished
fdebbe43 named in the old Session B2 document is absent from available object
stores; its published successor/history is retained, never recreated/invented.

Architecture: reuse the existing extracted-production native Vulkan harness and
independent byte-address oracle. First demonstrate stale retained HDR readers
with coherence omitted, then test image-only registration/transport without
changing the mixed gate. Separate exact logged-span rejection from synthetic
aligned/full-GOB positive controls. Audit publication precision, padding, budgets,
bootstrap and pending resources before considering any production extension.

Constraints: same PR400 branch only; PR453 read-only; no shaders/LOD/brightness,
new capture or diagnostic APK, merge, mixed/global activation or general refactor.

Tasks:
- [x] Verify remote heads, preserve published checkpoints and recover source.
- [x] Read PR453 audit/events and inspect its CheckLoggedPartialHistories.
- [x] Complete source audit of spans, versions, transport, image-only vs mixed,
  CPU publication/invalidation, image write precision and registration bootstrap.
- [x] Restore the host baseline: three aggregate assignments are ambiguous with
  the actual Vulkan-Hpp/clang19 headers; use explicit vk::Extent3D types only,
  preserving all assertions and production bodies. Re-run baseline before new tests.
- [x] Add a focused native HDR fixture with logged physical spans, live views,
  outside bytes, CPU upload/readback, pending operations and successive writes.
  Observe the negative stale-content failure before any renderer modification.
- [x] Prove existing image-only authority in bounded aligned HDR controls:
  nonuniform bit patterns, multi-mip metadata, publication masks, repeated writes,
  unchanged/exterior regions, preparation failures/retry and stable-use counters.
- [x] Test exact HDR edge/padding/budget refusals; never force unsupported spans
  through production guards. Scope partial-write claims to actual call contracts.
- [x] Run regional/mixed withdrawal regressions, ASan/UBSan, Vulkan validation,
  buffers/GMMU/aliases/submission/framebuffer; review and publish evidence on400.
- [x] Choose A/B/C from results. Runtime change/APK only if the full negative gate
  is correctly repaired within the proved scope; otherwise retain renderer/gates.

Review focus: late registration cannot infer per-mip writers from one serial;
physical padding may be another representation's live texels; a raw copy is not
proof of reversible general mixed transport; pending readers must retain backing
owners; per-draw whole-component/cell scans must not be called free performance.

Ruling: Outcome C for actual HDR integration — exact edge/partition admission
remains unproved, so publishing tests/evidence only costs another prerequisite
step but avoids treating the aligned positive as an Android fix.
Ruling: The extra conditional facade compile failure is baseline-identical and
is documented, not repaired or hidden in this bounded HDR capability change.

Final review: fresh read-only reviewer reran HDR green; no Critical/Important
findings. Factual minor wording corrected to describe allocation/preparation
injection accurately; no native recording-failure claim. Declined Android/layout/
late-bootstrap/consolidation/leak/conditional scopes remain blocked as documented.
No renderer change, APK, activation or merge authorized by this evidence.
