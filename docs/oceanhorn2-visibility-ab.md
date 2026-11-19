# Oceanhorn 2 — conditional visibility A/B diagnostic (no merge)

Status: **diagnostic only**, not a game fix. Branch is based on master
`a7bf81899e306f93e9db58a02f94235563051fb6`, which includes PR #317.

## Observation
- Strato: distant scene objects appear only when the player approaches.
- Eden: same issue not observed by the tester.
- Root cause **not established**. The guest could be performing LOD, streaming,
  occlusion culling, or conditional draws, and any of them can fail upstream.

## Artifacts from this PR's ordinary PR Build run
- `strato-pr<PR>-arm64-release.apk`: **CONTROL**. Unchanged guest predicate
  behavior, with rate-limited `[OCEAN-VIS-CONTROL]` messages.
- `strato-pr<PR>-arm64-force-visibility.apk`: **FORCED**. Bypasses only
  Maxwell's Conditional/RenderIfEqual/RenderIfNotEqual result in
  `Maxwell3D::CheckRenderCondition` while still honoring
  NeverRender/False. Emits `[OCEAN-VIS-FORCED]`.

Both are DevRelease arm64 APKs with the same package name. Install and test
**one at a time**; do not compare if save location, update, drivers, or settings
differ. The forced variant has intentionally incorrect graphics semantics and
can overdraw, flicker, or degrade performance. Do not use as a normal build.

Queries::Query also emits rate-limited `[OCEAN-VIS]` sample-report counters.
Counters are per native worker thread and show demand, **not** the actual GPU
query return value or whether a predicate was true/false.

## Reproduction
1. In CONTROL, choose one scene, position, camera angle, driver, and save.
   Wait for normal loading. Record whether specific trees/buildings/scenery
   are absent, and whether they disappear again on retreat.
2. Repeat using FORCED on the identical scene/position/camera. Also record
   glitches and FPS differences. Do not compare only shader-compilation time.
3. For both APKs export `emulation.log` and send a screenshot at the same
   viewpoint. The first relevant condition-check log and sample-report counts
   should appear without flood-level logging.
4. Optionally compare the same viewpoint with Eden (same game version).

## Interpretation
- Objects appear only in FORCED: suspect incorrect conditional draw gating;
  examine Maxwell reports/accumulation, 32/64-bit NonZero semantics, memory
  visibility, and Vulkan EXT conditional rendering on the Moto G100.
  This is **not** proof that the query counter itself is wrong.
- Objects remain absent in FORCED: prioritize frustum/distance culling and
  LOD/streaming decisions made by the guest, shader/geometry paths, indirect
  draws, and depth state. Do not expand the occlusion-query rewrite blindly.
- No `[OCEAN-VIS-CONTROL]` entries: the relevant gameplay may not use
  Maxwell conditional draws; this A/B probe will have limited diagnostic value.

## Scope and safeguards
- Flag `-PoceanVisibilityDiagMode={0,1,2}` maps to native macro
  `STRATO_OCEAN_VIS_MODE`. Defaults to 0 (no tracing/no override).
- Force mode is compile-time gated and confined to this PR's second APK.
- Do not merge. Keep PR #317 fixes, current texture/scheduler/CAS changes.
- Existing PR Build host regressions remain mandatory; the CI validates that
  the forced native library contains its distinct marker and the APK is signed.
- No on-device Oceanhorn 2 validation has occurred yet.
