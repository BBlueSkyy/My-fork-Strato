# BOTW regression bisection: pre-regional checkpoint

## Observed reproducible symptom
- PR #442 (complete Stage A port through `fb5bfc6`, with two later GMMU corrections): severe blue/cyan, red and yellow color/surface corruption in BOTW, user screenshot.
- PR #443 (same source except the `RegisterRegionAuthority` call is disabled): similar corruption persists; one screenshot shows entire display predominantly yellow/pink. This *does not* establish regional authority registration as the root cause.
- Both ran on device; no emulation log or matched game-content diff supplied, so a root cause is not yet proven.

## This A/B boundary
- Base is fixed master `505bc7d1a572f415032c908989a236c48b1ba533`.
- Port only the first nine commits of #400 through **`baaf3cd2bc97e49f8589d73d7127c66b95e07bc6`**, including pending source write/read ordering, texture upload authority, GPFIFO pushbuffer flush, preflight and overlap materialization. Merge current master edits into the five shared files using the original `9935f133` merge base. Preserve current master bindless/null texture changes.
- This build **excludes** all later regional authority work, GOB/3D expansion, GMMU segment/unmap changes, regional readback direction fix, subsequent stages and CBUF changes. No backport from the experimental #438; no shader-compiler or workflow change.
- No code from PR #400, PR #442 or PR #443 is modified; this is a distinct review-only branch.

## Decision
- **If corruption persists:** at least one of these nine early commits or their integration with current master is sufficient for the symptom. Bisect early upload/copy/overlap/preflight/GPFIFO independently.
- **If corruption disappears:** the regression is introduced after `baaf3cd2` or by a later interaction, including the later regional machinery still present in #443 (even with the registration call gated), GMMU changes, or linear readback. Bisect later group.
- This does NOT identify an exact bug, and should not be treated as a fix for MK8D/SMO.

## Validation and safety
Build ARM64 DevRelease, perform identical BOTW save/scene/camera, same driver, texture memory setting, and repeated boots. Include BOTW logs. Retest MK8D/SMO only after a proven fix; no merge or assumption that the two improvements are preserved in this diagnostic checkpoint.
