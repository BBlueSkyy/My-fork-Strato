# Stage C buffer/image sequencing implementation plan

> Implement incrementally in the existing Draft #400. Review each functional boundary before publication.

**Goal:** add a verified fully resident staged-buffer/image sharing domain without replacing image authority.

**Architecture:** retain the existing regional partition, resource locks, cycles and mapping publication lease. Add typed buffer ownership, raw sector transfers, selective CPU publication and migration of buffer bindings before enabling registration. A transfer changes residency; only an actual producer changes authority.

**Tech stack:** C++20, Vulkan, existing production-body host fixtures, lavapipe and synchronization validation.

**Scope:** live fully resident mappings; initially native RGBA8, dense 2D whole GOBs and staged buffers. Sparse, physical allocation reuse/B1b, DMI and general executor rollback remain outside this increment. Preserve all Stage A/B1a/Unmap fixes, #421 and the existing workflow.

## Review focus

- Partial writes must retain bytes owned by the other representation.
- CPU traps must never publish a stale whole buffer over a newer image.
- Coalescing must migrate authority and residency before retiring delegates/backings.
- A failed descriptor/preparation must not publish a buffer writer.
- Mapping changes must reject preparation before new nodes or authority.

## Steps

- [x] Restore runners after diagnostic cleanup and reproduce/fix cached buffer epoch reuse without an explicit purge. This is a prerequisite, not shared authority.

- [x] Prove bounded raw GOB-sector transfer geometry with actual Vulkan buffer → image → buffer and an independent byte oracle. Reject incomplete GOBs, invalid ranges and misaligned/overflowed buffer offsets before recording.
- [ ] Extend regional metadata with exclusive typed owners and buffer residency. Integrate selective buffer upload/readback and CPU traps before enabling mixed registration.
- [ ] Integrate cached bindings, deferred buffer writers, deterministic mixed resource acquisition and B1a mapping leases at operation boundaries.
- [ ] Migrate coalesced buffer bindings transactionally; retain old captured backing/cycles through pending commands.
- [ ] Exercise real bytes, partial writes/overlaps, CPU/GPU, multiple producers, cancellation, mapping changes and failures; then run existing GMMU/B1a/image regression suites and ASan/UBSan.
- [ ] Independently review the diff, publish validated commits in #400 only, and request one existing ARM64 DevRelease without monitoring Actions.

A sector-copy planner alone is groundwork, not enabled mixed authority. Registration must remain disabled until CPU publication, coalescing and producer commit gates pass together.
