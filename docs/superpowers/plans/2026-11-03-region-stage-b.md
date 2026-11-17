# Stage B — mapping identity and sparse sequencing plan

Spec: user's 2026-11-03 Stage B request. Start at remote `feb00c3d`;
master `ab2011a4`, clean local content identical to Stage A. Local Stage A
validation-only commits remain under `refs/validation/region-stage-a-local`.
Do not merge, touch #421, change workflows/shaders or advance to C/D.

## Sequential gates

B1 must distinguish translation validity from physical allocation identity.
A GMMU map/unmap event does not by itself destroy physical bytes: aliases through
other IOVAs may still own them. A CPU allocation release/reuse does invalidate
physical publication, including publication by commands queued before release.
No broad GMMU epoch alone is accepted as a physical allocation generation.

- [ ] Audit GMMU map/unmap, CPU memory unmapping/mirroring, nvmap lifetimes,
  descriptors, regional publication and queued command retention.
- [ ] Add deterministic failing physical identity/reuse and translation identity
  tests. Use real Vulkan images/bytes for old writer versus reused memory;
  metadata-only translation tests are supplementary.
- [ ] Implement bounded interval generation receipts with short validation /
  publication leases. Unchanged translation has no new event. Partial replacement
  preserves unaffected interval identities; removed/reused intervals never match
  the old receipt. Bound fragmentation and release obsolete history.
- [ ] Connect physical release to CPU allocation lifetime, distinguish FreeGuest
  eviction from actual deallocation, and prevent stale publication into reused
  pages while retaining old Vulkan commands/backings. Preserve exclusive bytes
  of partially invalidated old resources.
- [ ] Connect translation receipts/cache invalidation and resource preflight.
  Validate identity after resource acquisition and before authority publication;
  no waits while regional/identity publication metadata is held. Preserve the
  cold-lookup progress boundary and all Stage A checks.
- [ ] Validate B1 with current/reused/remapped addresses, pending GPU producers,
  content oracles, failures and affected regressions. Review before publication.
- [ ] Only after B1's central contracts pass, implement B2's bounded resident
  interval representation/partial transfers. Keep absent pages explicit and
  reject unsupported layouts instead of building an anonymous zero mirror.
- [ ] Only after B2 passes, validate B3 pending-remap lifetimes and B4 integration.
  Record a concrete architecture blocker if a gate cannot safely be completed;
  do not label B1–B4 or Stage B complete without their required data tests.

## Review focus

Physical identity versus IOVA identity; ABA reuse; partial invalidation losing
exclusive data; check/publication race; callback lock inversion; pending cycles
retaining old views; unchanged translations causing false events; metadata
limits and reclaimed old generations; cache retry progress; sparse holes
accidentally read/encoded as pixels. An unsupported case remains rejected.

## Validation and publication

Keep authority A→B→A at 0/1024, FreeGuest OFF/ON, both format-list modes.
Run full backing, lock-order, region-residency, texture-sequence, submission,
sampled-layout, storage-preflight, overlap-materialization and framebuffer
suites plus affected essentials, real Vulkan synchronization validation and
ASan/UBSan. Publish each validated functional substep to Draft #400 only;
request existing ARM64 DevRelease and obtain its link without following Actions.
Baseline game problems remain baseline; no game fixes/FPS claim.

## Audit ruling: first functional boundary B1a

Independent lifecycle review found that `UnmapMemory` alone cannot delimit
physical allocations: `KMemory::MapImpl/UnmapImpl` replace mappings with
`MAP_FIXED`; transfer/shared-memory restore and dynamic 36-bit release bypass
or precede that hook. CPU pointers and raw file descriptors can also alias or
be recycled. Installing that partial hook would make physical-generation claims
unsafe. Do not install it.

The first implementable functional slice is exact GMMU translation receipts,
cache retranslation and a short operation-publication validation lease. This
protects changed IOVA bindings and retains old physical images/views; it must
NOT retire physical writer history merely because an IOVA changed. B1a needs
real Vulkan P→Q→P bytes, partial replacement, pending view lifetime, no-op maps,
failed preparation and publication/retry. Full B1 allocation identity remains
blocked on a backing-ID/offset association transaction spanning the paths above.
B2–B4 stay gated until that distinction is integrated and validated.

## B1a execution record

Exact translation receipts, cache/pool/RT refresh and short publication guard
implemented. Independent review identified stale nested RT/pool caches, a
concurrent cached-validity race and repeated sparse-unmap false events. All
received deterministic negative controls/regressions and fixes. Real Vulkan
P→Q→P tests cover pending old readers, live physical writers and retention
failure. Full B1 allocation identity, B2, B3 and B4 remain blocked as detailed
in `docs/region-stage-b.md`; no UnmapMemory-only hook or physical identity claim.


### B1a publication gate

- [x] Verify remote HEAD/history/master and recover all unpublished source/tests.
- [x] Exact translation receipts and unchanged-map detection; cache/pool/RT refresh.
- [x] Independent review and deterministic validity/cache/RT/pool negative controls.
- [x] Final absent-tail ASan failure reproduced and fixed; exact map-body regression.
- [x] Final identity limits, failed split/snapshot recovery and owner-close tests.
- [x] Final authority ASan/UBSan, 0/1024 A→B→A, full backing, both lock probes,
  residence proof; prior affected essential regression results retained.
- [x] Document the full B1 physical allocation blocker, B2–B4 gates and device limits.

Only B1a is publishable at this gate. The full B1 checkboxes above remain open
where physical allocation identity/content tests are still absent. No partial
sparse image support or old-allocation publication protection is claimed.
