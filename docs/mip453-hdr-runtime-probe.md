# PR #453 — GPU HDR coherence routing audit (device-only)

This is a **bounded logging experiment**, not an HDR bloom fix. It is deliberately kept on the existing Draft PR without merging.

## Scope and exact evidence gate

The prior on-device #445 capture recorded overlapping guest allocations with identical physical start:
- 640x360 R11G11B10_UFLOAT, 1 mip / 983040 bytes: sampled by the bloom origin shader; pixels remained byte-identical across four readbacks.
- 640x360 same format, 2 mips / 1310720 bytes: separate GPU image receiving RT writes.
- Both had no AliasGroup. #453's host fixture repaired compatible whole-mip histories but the phone's actual promotion/rejection path was not previously observed.

Android-only `[GPU-MIP453]` messages are emitted through the normal `emulation.log` logger. No filesystem markers, GPU readbacks, screenshots, new passes, waits, descriptor changes, shader changes, or brightness operations. The filter is the **geometry and format** (640x360, R11G11B10_UFLOAT, 1 or 2 mips), **not fixed guest addresses or game identity**. A single atomically shared budget of 96 lines plus once-per-texture/event-category gating limits output. The diagnostic is deliberately specific to this device experiment; it must be removed before merging.

Events:
- `PREFIX ACCEPT/REJECT`: candidate child-first prefix and selected outcome. `reason_hint` is a **triage hint**, not a complete proof of the failing predicate. Compare block height/spacing and resource flags.
- `DONOR ACCEPT/REJECT`, `CONSOLIDATION_BLOCKED`, `FALLBACK_TO_SEPARATE_PARENT`: validates whether independent mip histories prevent consolidation; reject log includes safety flags but does **not** identify an exact single failure when multiple checks apply.
- `PROMOTION_DONE`: confirms an actual successful guest-prefix backing transaction and counts the donors; mere candidate discovery is insufficient.
- `NEW_IMAGE`: a distinct texture was allocated after lookup fallback; compare its address/span with the overlapping existing images.
- `RENDER_TARGET` and `SAMPLED`: actual render-pass view roles with native backing generation, guest physical span and mip view range. These are reported *once per texture/generation-class* and therefore **not per-frame shader records**; without a fragment-shader hash they do not prove an individual draw uses the sampled image.
- `VIEW_REFRESH`: a cached image-view handle was refreshed for a new backing generation; absent event does not prove stale descriptors if a new view was created instead.

**This log cannot prove actual pixel freshness.** A matched post-#453 native readback would be required *only if* the decisions show the intended source and writer have become the same native resource yet the output remains visually wrong. Never infer this from write serials or flags alone.

## Single phone validation

Install `strato-pr453-hdr-coherence-audit-arm64-release.apk` from PR #453's CI artifact, **only when the build succeeds and the artifact exists**. This PR's existing `dev` package suffix is `.mipcoherencefix`, so back up its app data before replacing an older #453 APK. Use Moto G100 with the same Turnip driver as the existing MK8 investigation, MK8 Stadium, and **Free Guest Texture Memory OFF**. Boot the scene once, let the start-grid/race show the abnormal bloom, and provide **only `emulation.log` and one screenshot**.

Interpretation is conditional:
- If `PREFIX REJECT` or `CONSOLIDATION_BLOCKED` + `NEW_IMAGE` occurs for the same guest range, the concrete gate must be analyzed, not bypassed.
- If `PROMOTION_DONE` occurs, correlate `SAMPLED` and `RENDER_TARGET` by **guest address and native texture pointer within that boot**. A different sampled object may remain alive; view-generation/descriptor assertions need verification.
- If they converge and bloom remains wrong, **do not claim success**: move to the HDR source values and its producer/shader path.
- Absence of filtered events could mean different descriptor dimensions, format, initialization order or logger budget, not that the renderer is correct.

No merge or production correction is authorized by log counts alone. Remove these diagnostics and re-run host Vulkan tests after a root-cause fix is demonstrated.
