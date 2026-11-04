# Stage B — B1a translation identity; physical lifetime gate remains open

## State and result

Start: Draft #400 `feb00c3d165a52224198163db51303f593aee0bf`.
Master inspected at `ab2011a42f629a6c458ade02fb4104a6d249cb69`; no merge.
Local Stage A tree was byte-identical to the remote. Local validation-only
Stage A commit objects remain under `refs/validation/region-stage-a-local`.
All Stage A commits, tests and safety mechanisms are preserved. #421 is untouched.
No shader compiler, workflow, buffer/image authority or general executor rollback
change is part of this work.

**Delivered: bounded B1a, exact GMMU translation identity and image descriptor
retranslation. B1 as a whole is incomplete. B2, B3 and B4 remain gated.**

An IOVA association and a physical allocation are different identities. Changing
IOVA P→Q must invalidate that descriptor's translation, but must not retire the
old physical image or erase its writer history: other IOVAs and already queued
commands can still legitimately use P. Conversely, replacing a CPU mapping or
freeing/reusing an allocation can invalidate physical publication without any
GMMU call. This substep implements the first distinction and explicitly does not
claim to solve the second.

Device baseline: SpongeBob, FighterZ and Zelda still have the reported existing
problems. They were not investigated, classified as new regressions, or claimed
fixed. There is no phone performance result.

## Coverage before / after

| Case | Before B1a | After B1a | Evidence / remaining boundary |
|---|---|---|---|
| Fully resident, unchanged image translation | Residence bool and numerical spans | Atomic translation-span/identity snapshot; stable receipt and no event for no-op Map/Unmap | Exact production map/translate bodies, unchanged subrange remap and stable interval generations |
| P→Q at the same IOVA and unchanged TIC | Index/store cache could return P based on unchanged header/channel sequence | Both cache fast returns validate interval receipts; retranslate Q | Real Vulkan new Q bytes compared against independent expected bytes; old-cache negative control fails on stale pixels |
| P→Q→P with the same live physical allocation P | Numerical address could revive the obsolete translation receipt | Each changed association gets a new generation; the old receipt stays invalid; physical P can still reuse its valid image/data | Real Vulkan red P / green Q bytes; old receipt never becomes current again. This is **IOVA ABA**, not freed-allocation ABA |
| Partial GMMU substitution | No generation proof | Only changed association intervals lose their identities; unchanged prefix/suffix retain generations | Production Map/Unmap/TranslateRangeTracked tests, including full-range remap over an already mixed range |
| Retained color/depth RT state; clear/DRAW_TEXTURE | Nested manual-dirty RT caches could keep the old view | Mapping revision invalidates nested RT states; direct clear getters and regular pipeline flush refresh them; draw/clear/DRAW_TEXTURE share epoch preparation | Exact RT refresh/getter bodies plus independent real Vulkan image bytes; RT negative control corrupts old P |
| TIC header pool remapping | Pool kept an untracked CPU span | Pool has its own receipt, all image/type/format/texel-header queries use a common checked getter, index owners are retained before replacement | Production pool flush/getter and real Vulkan consumers; pool negative control reads old header |
| TIC pool unused/nonresident tail | No per-interval proof, original first-block view could exceed its span | Expose only the contiguous resident prefix; absent entries stay inaccessible | Prefix-length assertion and unmapped pool rejection. This is descriptor-memory bounds protection, not partial sparse image sequencing |
| Change during operation preparation | No translation generation check at authority publication | Short epoch lease rejects the attempt before new writer/node publication; existing submit/retry boundary prepares again | Exact executor publication/retry bodies, cancelled preparation cleanup and two-attempt progress control |
| Pending old image/view readers | Physical image cycles retained, cache-entry raw pointers could outlive replaced owner | Shared cache owners plus attachment of retired views to the current recording cycle; no global GPU wait on Map | Real pending Vulkan read stays red after Q binding; two cache aliases; failed retention leaves the old entry intact |
| Sparse/nonresident image intervals | Stage A rejects a texture without full residence | Rejection preserved. Fragmented translation reports null gaps instead of non-null sparse placeholders | Residence and generation suites, ASan/UBSan; **no new partially resident image class admitted** |
| Freed/reused physical allocation, MAP_FIXED, shared/transfer backing | No allocation identity in regional history | Still blocked; no misleading UnmapMemory-only hook installed | Requires backing-ID/offset and association transactions across the paths below |
| B2 resident sparse authority; B3 physical pending-remap safety; B4 full integration | Not implemented | Not enabled | B1 physical allocation contract must be completed first |

## Meaning of a generation

`MappingIdentity` is owned by one address-space translator. Its generation
identifies a currently tracked **IOVA interval association**, not a CPU pointer,
file descriptor, Vulkan handle or physical allocation. Capture and translation
are serialized by the same GMMU table lock. Receipts contain immutable interval
identities; changed subintervals are removed/split without changing unaffected
identities. Fresh associations get monotonic identities. Destroying the
translator closes retained receipts.

`SameMappingLocked` compares translated addresses/residence semantics before
mutating a table. An unchanged Map, including an unchanged subrange, or repeated
Unmap causes no event. Null mappings are absent regardless of stale sparse bits
left in legacy block metadata. A real map operation computes only its differing
subranges before invalidation. Sparse placeholders are never returned as valid
physical spans by fragmented translation, and null spans are never used in
contiguity pointer arithmetic.

Receipts have a cached validity/revision fast path; a failed receipt stays
failed permanently. The concurrent-validator regression uses a scheduling hook
inserted only into a temporary test copy of the production header. The original
cached-false race fails deterministically; the corrected header passes. No
instrumentation or scheduling hook exists in production.

## Publication, retries and pending lifetimes

Cache validity belongs to each descriptor translation, not to `Texture`, because
several valid IOVAs can share one physical image. GMMU changes therefore do not
set that physical image `replaced`, drain its physical writer, or discard its
pending contents.

Preparation captures one small epoch value per operation. Stable cached uses
allocate no receipt and do not scan the GMMU. Just before writer/node publication,
the executor first owns the operation's full resources and state locks, then
acquires a short shared translation lease and the existing ordered regional
metadata. A changed epoch rejects preparation through the existing empty
reconciliation request: submit old work, release resources, then prepare again.
The old cold regional-lookup correction remains intact; no retry counter/timeout
or forced GPU completion was added.

Map/Unmap take the exclusive identity lease while mutating translation tables.
They never enter texture/resource/trap callbacks or wait on GPU completion. The
publication lease does not acquire the GMMU table mutex and ends before draw
hooks/general resource preparation. The old regional lock-order and real donor
producer tests continue to run.

Replacing a TIC/store or index entry attaches the old shared view before
changing either the view or receipt. A failed attachment leaves that entry
unchanged. The old image/view is retained through recording/cycle lifetime,
while new descriptors can use Q. Removing its IOVA does not suppress a legitimate
old writer's publication to still-live physical P/Q memory.

A failed GMMU mutation after identity invalidation conservatively invalidates
receipts; it does not claim the old receipt is current. This does not add general
rollback to legacy MapLocked/segment-table allocation failures or the executor.

## Concrete blocker for full B1 allocation identity

The current ledger keys CPU spans. CPU virtual addresses and file descriptor
numbers cannot universally identify backing allocations. `KMemory::host` and
its guest mapping can alias the same fd/offset; fd numbers and virtual addresses
can also be reused. The missing identity must be an object-owned monotonic
backing/allocation ID plus offset and a separately versioned CPU/IOVA association.

| Production path | Required physical lifetime transaction |
|---|---|
| `MemoryManager::UnmapMemory` → `FreeMemory`/MADV_REMOVE | Retire the logical base allocation before bytes are discarded; serialize against masked publication |
| `Texture::FreeGuest` | Preserve allocation identity during reconstructible eviction; do not confuse eviction with deallocation |
| `KMemory::MapImpl` / `UnmapImpl` | MAP_FIXED replacement must detach the old association before reuse, preserving live host/other aliases |
| `KSharedMemory::Unmap` / destructor | Replacement currently precedes IPC-lock-aware UnmapMemory handling; close that window atomically |
| `KTransferMemory::Unmap` / destructor | Restore Heap/Code mappings and copy-back under an allocation-association transaction |
| `ReleaseSharedMemoryBacking36Bit` and failure cleanup | Retire associations before direct munmap/release |
| `CreateMirror(s)` / mremap clone | Carry backing identity plus offset for each mirror piece, including fragmented mirrors |
| Ordinary and regional texture readback | Encode privately, mask by live physical generation under a short publication lease; preserve valid exclusive intervals after partial retirement |
| Regional component history / callbacks | Match writer/resident history by physical identity and offset; invalidate only retired physical cells; retain old backings for pending commands |

An UnmapMemory-only generation hook would leave the other replacement windows
unprotected and could discard valid aliases. It was deliberately not installed.
Full B1 cannot be marked complete with translation counters and helper tests.
B2 needs resident-only mirror/partial encode adapters after these contracts;
B3/B4 need actual allocation reuse with pending writers/readers and the production
replacement paths above. These are the next implementation gates, not evidence
that old physical publication is already protected.

## Bounds, costs and failures

- At most 65,536 current identity cells per translator and 4,096 fragments per
  receipt/change list. Overflow/invalid ranges are rejected explicitly.
- Unreferenced current cells are reclaimed at the cell ceiling; obsolete
  generations remain only in live receipts. Pending views/cycles release them
  normally. A stress test reaches the ceiling, rejects an excess live receipt,
  then succeeds after old receipts expire. It also rejects an extra split before
  mutation at the live-cell ceiling, preserves the old receipt on failure, and
  retries successfully after reclamation. A 4,097-fragment snapshot is rejected;
  a 4,096-fragment prefix remains valid afterward.
- Cached image use checks receipt atomics and a scalar epoch; there is no
  translation/cache-wide scan or new heap allocation on a stable draw. New
  receipts/interval cells allocate on translation/cache misses. Real changes
  traverse affected map blocks and actual descriptor/RT state.
- A GMMU change during a prepared operation retries that operation, even if its
  change was unrelated to a texture. Individual unchanged cache receipts remain
  valid. No all-GPU completion wait is introduced.
- Cache entries now retain a shared view and receipt. The identity interval map,
  bounded receipt vectors and transient retired-view dependencies increase host
  memory; no phone memory/FPS measurement has been made.

## Test evidence and scope

Host uses Clang 18, lavapipe and Khronos synchronization validation. Dependencies
remain outside the repository. All normal Stage A tests remain enabled.

RED controls observed:

- Original cache reuse ignores the changed receipt and returns red P when new
  Q bytes must be 0x73; real Vulkan byte assertion fails.
- Disabled nested RT refresh writes green into old red P; old-image byte oracle
  fails.
- Untracked pool lookup returns the old header after its IOVA changes; the
  header/consumer oracle fails.
- The original cached-false validity race returns true under a new revision;
  the adversarial two-validator test fails.
- Sparse→Unmap→identical Unmap produced a false new revision before null-state
  normalization.
- Final absent block after a resident descriptor prefix dereferenced the end
  iterator. ASan exposed it; a capacity-trimmed GMMU regression reproduces it,
  and fragmented translation now emits the bounded null tail without that read.

GREEN commands and final results are recorded below. Vulkan data tests cover
FreeGuest OFF/ON and both image-format-list modes. They execute exact production
GMMU map/unmap/translation functions, cache fast-return branches, mapping-cache
replacement helper, nested RT refresh/getters, pool flush/getter, executor
publication and retry. Allocation/mirror/graph/table storage are declared host
facades. Vulkan images, barriers, transfers, fences and downloaded bytes are
real. This is not a full Android executable, real shared/transfer memory release,
physical-allocation ABA protection, sparse image residency or an Android driver
performance claim.

## Changed files/functions

- `common/mapping_identity.h`: bounded interval receipts, generations, validation
  cache, short mutation/publication leases, owner close and metadata reclamation.
- `common/address_space.h/.inc`: exact/no-op changed-range detection,
  `TranslateRangeTracked`, Map/Unmap invalidation, null sparse/gap handling.
- `texture/mapping_cache.h`: descriptor association and transactional view retain.
- `interconnect/common/textures.h/.cpp`: tracked pool and all header readers,
  both TIC cache fast returns, view-owner retention and retranslation.
- `maxwell_3d/pipeline_state.h/.cpp`: nested RT mapping refresh and tracked color/
  depth translation; `maxwell_3d.h/.cpp`: shared draw/clear/DRAW_TEXTURE refresh.
- `fermi_2d.cpp`, `kepler_compute.cpp`: translation/operation receipt preparation.
- `command_executor.h/.cpp`: scalar mapping epoch, prepublication lease and
  cancelled/retried preparation cleanup.
- New `gpu_mapping_identity`, `gpu_gmmu_identity` tests; expanded real Vulkan
  backing tests; sampled-layout/draw-texture host facades remain compatible.

`region_authority.inc`, physical writer history, buffer ownership, kernel
allocation code, shader compiler, workflows and #421 were not modified.

## Final host validation (2026-11-03)

Recovered logs were inspected rather than rerunning completed negative controls.
Final-tree targeted runs after fixing the resident-prefix/absent-tail case:

| Command / scope | Result |
|---|---|
| `ASAN_OPTIONS=detect_leaks=0 python3 tests/gpu_mapping_identity/run.py` | PASS: concurrency scheduling, interval generations, lease exclusion, owner close, 65,536-cell ceiling, failed split preservation/recovery, 4,096-fragment receipt ceiling |
| `ASAN_OPTIONS=detect_leaks=0 python3 tests/gpu_gmmu_identity/run.py` | PASS: exact production functions, unchanged subranges, full-range substitution over mixed mappings, IOVA ABA, sparse/null gaps and final unmapped tail; ASan/UBSan |
| `TEXMAN_AUTHORITY_PROBE=1` backing runner with ASan/UBSan | PASS: original A→B→A **0/1,024 stale texels**, FreeGuest OFF/ON × format-list OFF/ON; all Stage A probes plus real translation-cache/RT/pool bytes, pending old read, retention failure, cancellation and publication retry |
| `tests/gpu_region_residency/run.py` | PASS after the tail fix: fast and fragmented residence proof; sparse placeholders remain absent |

Preserved earlier broad runs also passed: full `gpu_backing_promotion`, both
`TEXMAN_LOCK_ORDER_PROBE=1` modes, `gpu_texture_sequence`, `gpu_submission`,
`gpu_sampled_layout`, `gpu_storage_preflight`, `gpu_overlap_materialization`,
`gpu_framebuffer_cache`, `gpu_draw_texture`, `gpu_pushbuffer_sequence`,
`gpu_linear_readback` and `gpu_texman_multirange`. Full backing and both lock
probe modes were rerun on the final tree: PASS, zero Vulkan validation errors.

The backing runner requires Vulkan synchronization validation and asserts zero
validation errors. ASan leak checking is disabled because this host cannot run
LeakSanitizer task inspection; address and undefined-behavior checks remain on.
No Android compile completion or device run is implied. The unchanged PR Build
workflow requests ARM64 `assembleDevRelease` upon the branch synchronization;
its result is not awaited or monitored.
