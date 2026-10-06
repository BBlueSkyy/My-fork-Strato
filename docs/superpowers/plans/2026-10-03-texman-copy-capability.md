# Texman Directional Copy Capability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add metadata-only directional `ExactImageCopy` capabilities with race-safe synchronization plans.

**Architecture:** Preserve `CopyDependencyTracker` as the semantic equivalence/validity graph. Add generation snapshots to prepared reads and a separate `CopyCapabilityTracker` that validates and stores directional routes, prepares exact synchronization plans, and completes only successful non-raced plans.

**Tech Stack:** C++20 header-only texture metadata and host assertion tests.

**Spec:** `docs/superpowers/specs/2026-10-03-texman-copy-capability-design.md`

## Global Constraints

- Do not register CopyOnly in `TextureManager` or change runtime behavior.
- Preserve Full, #250, fallback, `Texture::replaced`, Vulkan, `UsageTracker`, `CommandExecutor`, and `BufferManager`.
- Reject 3D/depth-slice synchronization and all format/layout conversion.
- A directional route never implies its reverse.
- Metadata completion requires execution success and unchanged source/destination generations.

## Review Focus

- A source write after prepare must reject completion instead of importing its newer generation.
- A destination write after prepare must reject completion instead of overwriting newer authority.
- A reverse-only capability must not satisfy the requested direction.
- A direct semantic edge must be required even when image properties match.
- Registering or completing one mip/layer must not affect another endpoint.

---

### Task 1: Generation-bound prepared reads

**Files:**
- Modify: `app/src/main/cpp/skyline/gpu/texture/copy_dependency.h`
- Modify: `tests/texture/copy_dependency_tests.cpp`

**Interfaces:**
- Produces: generation fields on `PreparedDependencyRead`; `CompleteSynchronization(const PreparedDependencyRead<Representation> &)`.

- [ ] Write tests proving successful exact completion and rejection after a source or destination generation changes.
- [ ] Run `./tests/texture/run.sh` and verify the new assertions fail for the missing generation-bound API.
- [ ] Implement generation snapshots and atomic validation before changing the destination generation.
- [ ] Run `./tests/texture/run.sh`; expect `texture compatibility tests passed`.
- [ ] Commit as `texman: bind copy reads to subresource generations`.

### Task 2: Directional ExactImageCopy capability

**Files:**
- Create: `app/src/main/cpp/skyline/gpu/texture/copy_capability.h`
- Create: `tests/texture/copy_capability_tests.cpp`
- Modify: `tests/texture/run.sh`

**Interfaces:**
- Consumes: Task 1 generation-bound prepared reads.
- Produces: `CopyCapabilityTracker`, `CopyImageInfo`, `CopySynchronizationState`, and `PreparedCopySynchronization`.

- [ ] Write host tests for supported direction, reverse rejection, current/no-copy, stale/no-route, stale-source rejection, failed execution, generation races, exact-endpoint isolation, invalid properties, and missing direct edge.
- [ ] Run `./tests/texture/run.sh` and verify compilation fails because the capability API is absent.
- [ ] Implement the minimum directional registry, conservative validator, preparation, success-gated completion, and idempotent merge.
- [ ] Run `./tests/texture/run.sh`; expect `texture compatibility tests passed`.
- [ ] Commit as `texman: describe directional exact image copies`.

### Task 3: TextureGroup metadata façade and final verification

**Files:**
- Modify: `app/src/main/cpp/skyline/gpu/texture/storage.h`

**Interfaces:**
- Consumes: Tasks 1–2 tracker APIs.
- Produces: metadata-only `TextureGroup` registration, prepare, completion, and merge forwarding.

- [ ] Add forwarding methods without adding any runtime call site.
- [ ] Run `./tests/texture/run.sh`; expect `texture compatibility tests passed`.
- [ ] Verify `rg -n "RegisterExactImageCopy|PrepareCopySynchronization" app/src/main/cpp/skyline/gpu` shows definitions only and no `TextureManager`/executor call site.
- [ ] Run `git diff --check` and review `git diff 9690ba5b --`.
- [ ] Commit as `texman: expose copy capability metadata on texture groups`.
