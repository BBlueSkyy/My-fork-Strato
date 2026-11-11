# MK8D / SMO — isolated Stage A port from Draft #400

## Purpose and provenance

This is a **review-only candidate**, not a verified game-specific bug fix.
On-device observations reported for Draft #400:
- Mario Kart 8 Deluxe character rendering improved at checkpoint `1475633e` (historical run `41544961749`).
- Super Mario Odyssey fog improved at `feb00c3d` (historical run `41639774824`).
These are branch checkpoints, not necessarily the actual causative commits, and there is no established observation of both improvements in the **same** APK.

Candidate starts from **master `505bc7d1`**, and ports the bounded functionality and production fixtures through `fb5bfc6`, the implementation parent of `feb00c3d`. This includes the early resource sequencing, lookup retry, complete-GOB/planar-3D region logic, independent linear GPU readback direction, and regression fixtures. It also takes two separately proven GMMU fixes from #400 (segment-table L1 demotion tail and adjacent-unmap overwrite), with the production GMMU segment test. It is NOT a merge or cherry-pick of all of #400.

## Integration boundaries

- Eight files modified on master since the original merge base were reconciled against the frozen base, instead of overwritten. The recent TIC backing extent/null fallback, shader/pipeline, executor and texture changes from master are retained.
- A manual conflict in `interconnect/common/textures.cpp` preserves master's complete backing/minimum-backing retry while additionally requiring a fully resident GMMU translation. Both include insertions in `texture.cpp` are retained.
- Do **not** bring in experimental Stage B1/B2 or Stage C selective authority, the #438 smoke path, broad Buffer/Image mixed authority, global admission, unrelated shader-compiler work, workflow edits, or game-specific workarounds.
- Do **not** change Free Guest Texture Memory policy globally.
- This port does not include the later #400 CBUF `RefreshMapping()` correction (`dfa18b43`), because that fix depends on architectural changes not carried by this isolated stage. No implication that the later SMO crash is fixed.
- Historical reports include an SMO castle rendering issue already present before Stage A expansion. That regression must be tested explicitly and is NOT claimed to be resolved.

## Validation — merge gates

1. Static / build: Android ARM64 DevRelease build, native build integration, host fixtures and `git diff --check`.
2. ASan/UBSan fixtures: `gpu_texture_sequence`, `gpu_submission`, `gpu_pushbuffer_sequence`, `gpu_overlap_materialization`, `gpu_backing_promotion`, `gpu_storage_preflight`, `gpu_sampled_layout`, `gpu_linear_readback`, `gpu_region_residency`, `gpu_gmmu_segment` and other affected host GPU suites.
3. Vulkan synchronization validation where available: original A→B→A regional oracle with Free Guest OFF/ON, format-list ON/OFF, content/readback/failed-submit/retry invariants. Check no added GPU-wide completion waits on ordinary draws.
4. Device A/B at identical resolution, driver, game updates and shader-cache state:
   - MK8D: repeated boot, character selection (including after entering and leaving a race), cubemap/character appearance, gameplay and preview.
   - SMO: fog/waterfall, castle interior, scene transitions, idle/repeat boot, CBUF crashes and freezing.
   - Both: Free Guest OFF and ON, FPS/frametime, peak memory and frequent transitions; compare against the unmodified master and the relevant historical checkpoints.
5. Reject/keep Draft if Android build fails, any new corruption or stall appears, or repeated tests fail. No merge solely because a host fixture passes.

This document records a port and its gate; **it is not proof that the visuals are fixed on current master**.
