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
| CopyOnly/maintenance5 | No new copy routes, Vulkan features or experimental depth-slice activation. Existing shared-image views remain intact. |

Implementation plan:
- [x] 1. Reuse V2 guest-range helpers, replace end-only mapping comparison and cross-segment pointer subtraction, and reconcile equivalent partitions through the existing V3 authority group. Add failing fragmented-mapping/alias regressions; validate Vulkan and pitch tests; publish.
- [x] 2. Reuse the storage/index boundary, cache physical layouts and deduplicate overlaps across every segment. Test nested mappings, later-segment-only overlap, precedence and disjoint queries; validate production manager, #340/#341 and V3 tests; publish.
- [x] 3. Fresh review, fix material findings with regressions, publish any fixes. Request ARM64 builds through normal pushes; report SHA and run URL only. Stop for device tests.

Constraints: no NCA/kernel/audio changes; retain UsageTracker, real writer hooks,
transfer rollback, padding protection, stale/retired alias gates and Vulkan 1.1.
No repeated audits or CI-log analysis. Frame-time/RAM effects need device tests.

Validation: the production manager failed the fragmented-parent and later-segment
readback regressions before their fixes. The current methods pass real Vulkan
content/authority/view tests with synchronization validation and UBSan. The index
is checked against a full overlap oracle for 300 two-segment storages and 3000
queries, including nested intervals, endpoint exclusion, deterministic tie order
and retained storage lifetime. Query bounds use start-address and prefix-end
binary searches; parent ranges are cached once, and each storage is matched once
per lookup. Insert reserves all capacity before publishing a resource's segments.

Review fix: a later-segment overlap could read guest bytes before an attached
writer's queued nodes were submitted. The fallback now raises the existing V3
reconciliation request for attached writers with unencoded writes. The executor
submits first, locks and reads back those sources, then retries preparation.
Available guest bytes remain pinned through submission/readback; already
freed sources can be reconstructed by native readback. Clean overlaps and
representable shared views do not gain a submission. CopyOnly stays disabled.

The pending-writer regression failed before this fix and passed afterward. It
executes real Vulkan writes at the test executor's submission boundary and checks
readback order, pin lifetime, retry progress and no extra submit on a second
clean lookup. Android's noncontiguous mirror and command-node transport remain
explicit host-test boundaries; this is not an end-to-end Android executor test.
The nested interval regression also resolves the longer parent's second mip past
an intervening shorter mapping. No other material review findings remain.
