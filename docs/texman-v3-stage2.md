# TexMan V3 — stage 2

References: master 66637da0, V3 9bbbf96e, V2/#244 0a4a6c94.
Continue wip/texman-v3/#353; no merge, no new PR, no CI polling.

| V2 component | Stage-2 decision |
| --- | --- |
| GuestResourceRanges | Reuse complete ordered physical containment and logical offsets; accept equivalent mapping partitions only when every byte matches. |
| TextureMappingCache | Reuse all-segment indexing, unique storage candidates and deterministic scan order; add bounded indexed searches instead of V2's whole-table scans. |
| TextureStorage | Reuse transitional texture ownership and cached immutable physical ranges. Keep backing and authority in Texture. |
| TextureGroup | Do not duplicate V3 AliasGroup authority or import CopyOnly generations/capabilities. |
| Full classifier | Preserve V3 bounds, dimensions, strides, mip growth and view-format validation. V2's legacy equal-allocation-size predicate would regress #341. |
| CopyOnly/maintenance5 | No new copy routes, Vulkan features, submissions or experimental depth-slice activation. Existing shared-image views remain intact. |

Implementation plan:
- [x] 1. Reuse V2 guest-range helpers, replace end-only mapping comparison and cross-segment pointer subtraction, and reconcile equivalent partitions through the existing V3 authority group. Add failing fragmented-mapping/alias regressions; validate Vulkan and pitch tests; publish.
- [ ] 2. Reuse the storage/index boundary, cache physical layouts and deduplicate overlaps across every segment. Test nested mappings, later-segment-only overlap, precedence and disjoint queries; validate production manager, #340/#341 and V3 tests; publish.
- [ ] 3. Fresh review, fix material findings with regressions, publish any fixes. Request ARM64 builds through normal pushes; report SHA and run URL only. Stop for device tests.

Constraints: no NCA/kernel/audio changes; retain UsageTracker, real writer hooks,
transfer rollback, padding protection, stale/retired alias gates and Vulkan 1.1.
No repeated audits or CI-log analysis. Frame-time/RAM effects need device tests.
