# Texman v2 Subresource Validity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace representation-wide CopyOnly authority with a metadata-only graph that tracks current and stale state independently for each `(mip, layer, depthSlice)` endpoint.

**Architecture:** `resource_layout.h` will describe CopyOnly relationships as explicit subresource pairs. `CopyDependencyTracker` will store weak representation nodes, exact subresource endpoints with generations, and direct edges between proven equivalent regions; all write, read, synchronization, and merge operations remain metadata-only.

**Tech Stack:** C++20 header-only texture metadata, `std::shared_ptr`/`std::weak_ptr` ownership identity, existing shell-driven host tests with `-Wall -Wextra -Werror`.

**Spec:** `docs/superpowers/specs/2026-10-02-texman-subresource-validity-design.md`

## Global Constraints

- Do not modify or activate `TextureManager`, Vulkan copy execution, #250, Full selection, legacy fallback, `Texture::replaced`, `UsageTracker`, `CommandExecutor`, or `BufferManager`.
- CopyOnly registration continues to require `IsCompleteGuestAlias`; physical partial overlap is insufficient.
- Endpoint identity is representation ownership identity plus exact mip, layer, and depth slice.
- Queries never promote stale endpoints.
- Registration and batched writes are atomic on validation failure.
- `nextGeneration` remains strictly greater than every local or imported endpoint generation.
- Validate every registered coordinate against the real layout and the depth of that exact mip/layer.

## Review Focus

- Higher 3D mips have smaller depth: registration must reject a slice valid only at mip zero. Covered by Task 2 layout-bound tests.
- Duplicate coordinates in one write batch must not consume or assign multiple generations. Covered by Task 3 deduplication tests.
- One invalid mapping in a multi-pair relationship must leave every endpoint untracked. Covered by Task 2 atomic-registration test.
- An expired authoritative owner must remain distinguishable from a new allocation and must not promote stale peers. Covered by Task 4 lifetime test.
- A metadata merge with generations ahead of the receiver must advance the receiver's allocator before its next write. Covered by Task 5 merge test.

---

### Task 1: Resolve Exact CopyOnly Subresource Pairs

**Files:**
- Modify: `app/src/main/cpp/skyline/gpu/texture/resource_layout.h`
- Modify: `tests/texture/resource_layout_tests.cpp`

**Interfaces:**
- Produces: `ResolvedSubresource { uint32_t mip, layer, depthSlice; }` with equality.
- Produces: `ResolvedCopySubresource { ResolvedSubresource backing, requested; }`.
- Produces: `ResolvedCopyRegion::subresources` as `std::vector<ResolvedCopySubresource>`.
- Produces: `ContainsSubresource(const TextureResourceLayout &, ResolvedSubresource) -> bool`.
- Preserves: `ResolvedViewBase`, `ResolveFullView`, and `ConfirmFullViewAgainstLegacy` behavior.

- [ ] **Step 1: Write failing CopyOnly mapping tests**

  Update `resource_layout_tests.cpp` to assert that the existing CopyOnly mip fixture produces one explicit pair `(backing mip 1, layer 0, slice 0) -> (requested mip 0, layer 0, slice 0)`, and add a selected multi-layer fixture whose result contains one pair per selected layer.

- [ ] **Step 2: Write failing layout-bound tests**

  Add assertions that `ContainsSubresource` accepts valid mip/layer/slice coordinates, rejects missing mips and layers, and rejects a depth slice equal to the selected mip's `GuestSubresource::depth`.

- [ ] **Step 3: Run the host tests and confirm RED**

  Run: `tests/texture/run.sh`

  Expected: compile failure because the explicit subresource types and mapping list do not exist.

- [ ] **Step 4: Implement explicit resolved mappings**

  In `resource_layout.h`, add the three types above, implement `ContainsSubresource`, and make the CopyOnly branch of `ClassifyAndResolveView` build an exact pair for every selected requested mip/layer. Reject ambiguous or incomplete resolution as `LayoutIncompatible`. Leave the Full branch on `ResolveFullView` unchanged.

- [ ] **Step 5: Run tests and confirm GREEN**

  Run: `tests/texture/run.sh`

  Expected: all texture host tests pass.

- [ ] **Step 6: Commit**

  ```bash
  git add app/src/main/cpp/skyline/gpu/texture/resource_layout.h tests/texture/resource_layout_tests.cpp
  git commit -m "texman: resolve exact CopyOnly subresources"
  ```

### Task 2: Register a Validated Multi-dependency Graph

**Files:**
- Modify: `app/src/main/cpp/skyline/gpu/texture/copy_dependency.h`
- Modify: `tests/texture/copy_dependency_tests.cpp`

**Interfaces:**
- Consumes: `ResolvedSubresource`, `ResolvedCopyRegion::subresources`, and `ContainsSubresource` from Task 1.
- Produces: `RegisterSynchronized(backing, backingRanges, backingLayout, requested, requestedRanges, requestedLayout, classified) -> bool`.
- Produces: multiple direct edges per representation while keeping unrelated endpoint components separate.

- [ ] **Step 1: Replace the single-dependency test with failing graph-registration tests**

  Add fixtures for two mips, two layers, and two 3D mip depths. Assert that one storage can register dependencies with two peers for the same exact region and independent dependencies for different regions.

- [ ] **Step 2: Add failing validation and atomicity tests**

  Assert rejection for partial physical overlap, duplicate/conflicting mappings, out-of-range mip/layer, a depth slice outside the exact mip depth, and a multi-pair relationship containing one invalid coordinate. After every rejection, assert that the otherwise-valid endpoint remains `Untracked`.

- [ ] **Step 3: Run tests and confirm RED**

  Run: `tests/texture/run.sh`

  Expected: compile failures from the new registration signature and subresource state query.

- [ ] **Step 4: Implement ownership nodes, endpoint states, and direct edges**

  Replace representation-wide nodes with exact endpoint states. Preserve weak control-block identity after expiration. Validate the complete relationship on a temporary tracker copy, including both real layouts, before committing it to `*this`. Support new-to-new, current-to-new, and current-component-to-current-component synchronized registration; reject stale endpoints and contradictory mappings.

- [ ] **Step 5: Add the exact state query**

  Implement `GetState(const std::shared_ptr<Representation> &, ResolvedSubresource) -> CopyRepresentationState`. Determine current/stale from the maximum generation in only that endpoint's connected component.

- [ ] **Step 6: Run tests and confirm GREEN**

  Run: `tests/texture/run.sh`

  Expected: all texture host tests pass.

- [ ] **Step 7: Commit**

  ```bash
  git add app/src/main/cpp/skyline/gpu/texture/copy_dependency.h tests/texture/copy_dependency_tests.cpp
  git commit -m "texman: register subresource copy graphs"
  ```

### Task 3: Mark Subresource Writes Atomically

**Files:**
- Modify: `app/src/main/cpp/skyline/gpu/texture/copy_dependency.h`
- Modify: `tests/texture/copy_dependency_tests.cpp`

**Interfaces:**
- Consumes: exact endpoint graph and `GetState` from Task 2.
- Produces: `MarkWritten(const std::shared_ptr<Representation> &, std::span<const ResolvedSubresource>) -> bool`.

- [ ] **Step 1: Write failing granularity tests**

  Assert independently that writing one mip, one array layer, or one 3D depth slice makes only the equivalent endpoints stale while unrelated coordinates retain their state.

- [ ] **Step 2: Write failing latest-authority tests**

  Alternate writes between two representations of the same exact region and assert after each write that only endpoints with the newest generation are `Current`.

- [ ] **Step 3: Write failing batch-atomicity tests**

  Pass a batch containing duplicates and assert the write succeeds once for each unique endpoint. Pass batches containing an invalid coordinate, an untracked coordinate, and an empty span; assert failure and verify no previously current/stale state changed.

- [ ] **Step 4: Run tests and confirm RED**

  Run: `tests/texture/run.sh`

  Expected: compile failure because only representation-wide `MarkWritten` exists.

- [ ] **Step 5: Implement two-phase batched writes**

  First validate the representation, every coordinate, and every tracked endpoint while building a deduplicated endpoint list. Only then allocate one generation and assign it to the unique endpoints. Keep `nextGeneration` as the next unused value, not the last allocated value.

- [ ] **Step 6: Run tests and confirm GREEN**

  Run: `tests/texture/run.sh`

  Expected: all texture host tests pass.

- [ ] **Step 7: Commit**

  ```bash
  git add app/src/main/cpp/skyline/gpu/texture/copy_dependency.h tests/texture/copy_dependency_tests.cpp
  git commit -m "texman: track writes per texture subresource"
  ```

### Task 4: Prepare Reads and Complete Only Direct Synchronizations

**Files:**
- Modify: `app/src/main/cpp/skyline/gpu/texture/copy_dependency.h`
- Modify: `tests/texture/copy_dependency_tests.cpp`

**Interfaces:**
- Produces: `PrepareRead(representation, subresource) -> PreparedCopyRead<Representation>` containing the exact source and destination subresources.
- Produces: `CompleteSynchronization(destination, destinationSubresource, source, sourceSubresource) -> bool`.

- [ ] **Step 1: Write failing direct-read tests**

  Assert `Current` for a current endpoint, `SynchronizationRequired` only from a directly connected authoritative endpoint, and unchanged stale state before and after repeated `PrepareRead` calls.

- [ ] **Step 2: Write failing direct-completion tests**

  Assert that completion updates only the exact destination pair, rejects a stale source, rejects reversed or unrelated coordinates, and leaves other mip/layer/slice endpoints untouched.

- [ ] **Step 3: Write failing transitive and lifetime tests**

  Build `A <-> B <-> C`, write A, and assert reading C is `Unavailable` until B is synchronized from A. Destroy the sole authoritative representation and assert live stale peers stay `Stale` and `Unavailable`; construct a new allocation and assert it is not mistaken for the expired owner.

- [ ] **Step 4: Run tests and confirm RED**

  Run: `tests/texture/run.sh`

  Expected: compile failure from the new per-subresource read and completion signatures.

- [ ] **Step 5: Implement direct read preparation and completion**

  Traverse the exact endpoint component to find its current generation, but accept as a copy source only a live endpoint connected by one registered direct edge. `PrepareRead` performs no mutation. Completion copies only the source generation to the named destination endpoint after validating the directed mapping and current source.

- [ ] **Step 6: Run tests and confirm GREEN**

  Run: `tests/texture/run.sh`

  Expected: all texture host tests pass.

- [ ] **Step 7: Commit**

  ```bash
  git add app/src/main/cpp/skyline/gpu/texture/copy_dependency.h tests/texture/copy_dependency_tests.cpp
  git commit -m "texman: require direct subresource synchronization"
  ```

### Task 5: Merge Metadata and Expose TextureGroup Wrappers

**Files:**
- Modify: `app/src/main/cpp/skyline/gpu/texture/copy_dependency.h`
- Modify: `app/src/main/cpp/skyline/gpu/texture/storage.h`
- Modify: `tests/texture/copy_dependency_tests.cpp`

**Interfaces:**
- Produces: idempotent `CopyDependencyTracker::MergeFrom` for nodes, endpoints, edges, and generations.
- Produces: subresource-granular `TextureGroup` wrappers matching Tasks 2–4.

- [ ] **Step 1: Write failing merge tests**

  Merge a tracker whose current endpoints have generations ahead of the receiver, repeat the same merge, and assert relation count and state are unchanged by repetition. Perform the next write and assert its endpoint becomes uniquely current, proving `nextGeneration` is greater than every imported generation.

- [ ] **Step 2: Run tests and confirm RED**

  Run: `tests/texture/run.sh`

  Expected: merge-state or generation-order assertion failure.

- [ ] **Step 3: Implement graph merge with a strict allocator invariant**

  Deduplicate nodes by weak ownership identity, endpoints by node plus subresource, and edges by exact endpoint pair. Preserve endpoint generations. Set `nextGeneration` to at least one greater than the maximum generation found in either tracker after every merge.

- [ ] **Step 4: Replace representation-wide TextureGroup wrappers**

  Update `storage.h` so registration receives both real `TextureResourceLayout`s and all state/write/read/completion methods require exact subresources. Keep `JoinTextureStorageGroups` metadata-only and preserve its existing group selection behavior.

- [ ] **Step 5: Run tests and confirm GREEN**

  Run: `tests/texture/run.sh`

  Expected: all texture host tests pass.

- [ ] **Step 6: Commit**

  ```bash
  git add app/src/main/cpp/skyline/gpu/texture/copy_dependency.h app/src/main/cpp/skyline/gpu/texture/storage.h tests/texture/copy_dependency_tests.cpp
  git commit -m "texman: preserve subresource validity across groups"
  ```

### Task 6: Final Scope and Regression Verification

**Files:**
- Verify only; no planned production changes.

**Interfaces:**
- Verifies all interfaces and constraints from Tasks 1–5.

- [ ] **Step 1: Run the complete texture host suite**

  Run: `tests/texture/run.sh`

  Expected: exit 0 with every texture test binary passing.

- [ ] **Step 2: Run whitespace validation**

  Run: `git diff --check e8da88d9e0e1f847a154d328370321d613b70cc9..HEAD`

  Expected: no output.

- [ ] **Step 3: Review the final scope**

  Run: `git diff --name-status e8da88d9e0e1f847a154d328370321d613b70cc9..HEAD` and `git diff --stat e8da88d9e0e1f847a154d328370321d613b70cc9..HEAD`.

  Expected: only the spec/plan, texture metadata headers, and texture host tests changed. Confirm there is no diff in `texture_manager.cpp`, Vulkan code, `Texture`, #250 logic, Full selection, `UsageTracker`, `CommandExecutor`, or `BufferManager`.

- [ ] **Step 4: Confirm branch cleanliness and commit history**

  Run: `git status --short` and `git log --oneline e8da88d9..HEAD`.

  Expected: clean worktree and small semantic commits matching the tasks above.

- [ ] **Step 5: Publish with a checked remote head**

  Fetch `origin/wip/texman-v2`, require its head to remain `e8da88d9e0e1f847a154d328370321d613b70cc9` before the update, force-update the same branch with lease semantics, confirm PR #244 remains Draft, and stop without reading workflow/build status.
