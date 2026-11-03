# Regional sequencing expansion — validated bounded stage (2026-11-03)

## Repository state and scope

Started from remote Draft #400 HEAD `1475633e58f9480411d0b1a3cde841f0b15e6138`.
Remote master was `ab2011a42f629a6c458ade02fb4104a6d249cb69`; merge base
`9935f13349c00d732ae599f4f90866b5b79629f5`. Fresh checkout, no previous local
changes. All 15 existing branch commits are preserved. Master was inspected,
not merged; its loader, index-binding and shader-compiler changes are not
reverted or replaced by this diff. #421 and its instrumentation are untouched.

This is a functional bounded expansion of Stage A, residence-proof groundwork
for Stage B, and limited transfer/drain failure protection relevant to Stage D.
It is **not** completion of all four architectural stages. No game-specific
correction or Android FPS claim is made.

## Coverage matrix

| Resource / case | Previous coverage and code restriction | New coverage / invariant | Corruption risk | Implementation strategy | Acceptance evidence / remaining gate |
|---|---|---|---|---|---|
| Native dense 2D block-linear rows | Supported; matching format, dimensions, tiling, mip/layer counts; aligned physical rows | Preserved fast path, including native BC1 partial format-block width | Incorrect row stride can misaddress compressed uploads | Retain whole-row spans; round explicit packed buffer strides to format blocks | Original Vulkan A→B→A: 0/1024, FreeGuest OFF/ON; BC1 29×64 actual compressed bytes and synchronization validation |
| Mips, layers, fragmented mappings | Already supported when every intersection is a compatible whole row | Preserved; fragments can additionally end at complete GOB boundaries in the new path | Confusing mip-major staging with layer-major guest storage | Each binding retains physical offset, mip, layer and explicit image coordinates | Existing dense mip/layer and fragmented suites; new patterned mips/layers and CPU repair |
| GOB padding / different block heights / physical footprints | `PhysicalImageSpans` rejected `blockLinearSize != linearSize`; registration required identical layouts | Complete represented GOBs, up to 4096 per member; native equal Vulkan format; physical intersections must cover complete corresponding GOBs | Padding falsely published as pixels; newer pixel data overwritten by an older image's padding | Keep dense path; use x/y/z/width GOB spans when needed; omit unrepresented padding entirely; masked readback; region image copies and strided partial uploads | Distinct per-position patterns across A→B→A; block heights 4/2; TIC width spacing; mip/layer gaps and tails; full guest-byte comparison including padding; failure/retry and no repeated repairs |
| Partial intersections smaller than a ROB | Rejected unless full-row aligned | Whole-GOB intersections admitted, including identical layouts shifted 512 bytes and physically contained non-view resources | Wrong x/y correspondence or missed contained producer | Validate physical endpoints and copy rectangle geometry; collect contained overlaps that cannot share a view | Same-layout shift, differing extents, containment and existing cold retry controls |
| Arbitrary partial GOBs / partial edge GOBs | Outside row proof | Remain rejected; no arbitrary bytes treated as texels | Sector/texel fragmentation, partial compressed blocks, unreconstructible bytes | Existing drain/materialization paths; refuse drain if a writer cannot really be published | Rejection controls for partial 256-byte mapping endpoints, partial width, self-aliasing; existing fallback data tests |
| Planar 3D, blockDepth=1 | Regional proof required image type 2D and depth 1 | Explicit z per slice; mip depth can shrink; compatible 3D↔3D and 3D↔2D-array physical intervals | Copying a different slice, using z as array layer, wrong packed mip offset | Separate regional reversible proof from exact-alias eligibility; per-slice copies, correct source/destination subresource ranges | Actual Vulkan patterned volumes and mips; mixed 3D/2D-array; independently addressed byte oracle; FreeGuest OFF/ON |
| Interleaved-Z 3D / depth aspects / converted formats / MSAA | Not reversible in this regional domain | Remain rejected | Missing/non-invertible guest encoding and illegal copy geometry | Preserve existing TextureManager slice-view reuse and safe rejection/fallback | Z-interleaving rejection, existing sampled layout/slice geometry tests; a generalized Z footprint and real-data tests remain required |
| Sparse / partly resident resources | Pointer-only `MappingsValid` could not establish physical residence, especially fragmented translation | Translation now reports whether **all** intervals are resident under the same GMMU lock; texture producers carry the proof; nonresident textures cannot enter regional authority | Non-null sparse placeholders acquire false physical identity or get mirrored/copied | Optional `fullyResident` output in fast and fragmented translation; `GuestTexture::MappingsValid` includes that proof; existing null/disabled texture handling remains the fallback | Production translation bodies test resident fragments, non-null sparse placeholders, unmapped blocks and changed residence; rejected admission allocates no image. **Resident subsets of sparse images are not sequenced yet** |
| Buffer ↔ image overlap | BufferManager coalesces buffer backings and tracks buffer cycles; no shared texture authority index | Not enabled | A buffer can contain newer bytes than guest/image authority; direct imported memory and staged mirrors have different publication rules | Next stage needs a physical interval registry with resource-kind-specific transfer adapters and operation read/write receipts | Must test actual storage-buffer GPU writes→texture reads and reverse publication; existing transfer staging buffers do not prove general buffer/image coherence |
| Remap, deallocation and physical identity reuse | Texture ledger keys physical CPU spans; backingGeneration protects Vulkan views, not guest allocation identity | Residence snapshot only; full generation/lifetime protocol remains absent | Same numerical address may be a different allocation; cached old descriptors may survive remap | Need GMMU/process-memory generation receipts, invalidation of retained descriptors and deferred old backing lifetime; no address-only generation guess | Real remap with pending GPU consumers, page reuse, split/remove authority and descriptors remains a blocking gate |
| Intermediate transfer/drain failure | Transfer residency committed after successful submit; no complete executor rollback | Preserve actual snapshot versions across unlocked I/O; retry changed producers; drain validates every unpublished owner before detaching any member | False residency, silently dropped non-reversible writers, partial component detachment | Prepare allocations/copies; submit; commit only versions actually copied; two-pass drain validation; reject failed readback | Allocation/attachment/recording/submit failures and retry; historical converted writer rejection retains state and bytes; new producer during transfer is reconciled |
| General draw/dispatch rollback | Publication guard validates inputs before nodes/writers but not all later allocations | Still incomplete | An exception after graph/writer publication may leave an uncommitted operation visible | Requires executor node/callback/descriptor allocation transaction and cancellation receipts, not a local version reset after GPU submission | Whole AddSubpass/CreateRenderPass control paths and existing allocation tests pass; all post-publication failure sites still need a dedicated transaction project |

## Protocol and transaction boundaries

Registration computes/validates all footprints, partitions, bindings and neighbor
lists before publishing a new component. The dense proof remains preferred.
GOB setup has an explicit 4096-GOB budget per member; larger or unproved cases
retain the fallback. Neighbor intersection uses sorted binding cursors; old
regions and resident bindings use binary lookup instead of quadratic GOB scans.

Reconciliation snapshots donors, acquires resources first and state mutexes in
address order, then checks component/donor identity. It releases regional
metadata before guest upload, staging preparation, GPU submission waits and
transfer submission. It revalidates donors after initialization and commits
only the snapshot versions actually copied. A peer becoming a new writer while
metadata is unlocked triggers another bounded-resource reconciliation pass.
No retry count, global GPU completion wait or per-draw manager/cache scan is
introduced. Sources retain the actual copy-reader cycle and backing lifetime.

Regional uploads record their guest version snapshot, rather than newer
metadata observed after the upload. Readback encodes privately; publication
reacquires metadata only to mask bytes still owned by that resource. Padding
omitted by the footprint has no image writer and is never copied from an
uninitialized encoding. FreeGuest retains regional guest bytes and padding.

CPU trap preparation performs required regional readback **outside trapMutex**.
Read/write callbacks retain try-lock semantics. A later producer causes false
/retry instead of a blocking metadata reacquisition under trapMutex. This avoids
a metadata→TrapRegions / trapMutex→metadata inversion. Resource/state locks may
still cover resource-local transfer work; regional metadata does not cover its
GPU waits. The short draw/dispatch publication guard remains unchanged.

Drain first attempts publication, then checks all owners before detaching any
member. A converted/undefined writer whose readback was skipped cannot be
reported as drained. Already successful guest publications need no rollback;
unpublished ownership remains protected after rejection. This is not general
rollback of executor nodes or GPU work already submitted.

## Red/green evidence and validation

Host: Clang 18, Vulkan-Hpp/header 275, Mesa lavapipe, Khronos validation with
synchronization validation enabled. Dependencies live outside the repository.
The Android pinned headers/compiler/workflows are unchanged.

Observed RED before the relevant correction:

- New padded/block-height cases had no regional authority.
- Identical dense layouts with a GOB-shifted intersection were rejected.
- A contained non-view image was omitted by TextureManager; direct registration
  proved the footprint itself eligible, isolating lookup collection.
- A wait hook on another thread could not acquire regional metadata during a
  transfer wait. Regional trap prepare also left dirty GPU data unpublished.
- Native BC1 29×64 CPU repair produced Vulkan VUID 09106 (row length 29).
- The historical converted writer regression caught drain changing its state
  after a skipped publication; the two-pass owner check prevents detachment.
- Sparse residence output was absent; new production-body tests exercise its
  fast and fragmented paths and rejection gate.

Final validation commands/results are recorded after all fixes below. Real
Vulkan data checks compare packed downloads and full guest bytes against an
independent Maxwell address oracle. Distinct patterns exercise x/y/z, mips,
layers, padding-as-other-resource-pixels, exclusive data and CPU repair. The
original A→B→A oracle is preserved unchanged at 0/1024 for both FreeGuest modes
and both image-format-list modes. The connected-component boundary test covers
three 4096-GOB members (host extension around 38–48 ms in these runs; not a phone
measurement).

The suites still use explicit host facades for allocation/traps/fences/executor
graph boundaries. Vulkan allocations, image/buffer copies, barriers, submits
and downloaded bytes are real. This is not a complete Android executable,
actual MADV_REMOVE testing, a full guest compute dispatch, GPU sparse residency,
or pending-remap device validation. A pre-existing `gpu_linear_readback` facade
lacked `Level::blockLinearSize` referenced by the production body; its missing
field was supplied without changing the readback assertions.

## Files and function responsibilities

- `texture/region_authority.h`, `texture/texture.h`: explicit GOB/slice binding
  geometry, regional upload snapshot API, texture residence proof.
- `texture/region_authority.inc`: `PhysicalImageSpans`, `RegisterRegionAuthority`,
  `ReconcileRegions`, `RecordRegionUpload`, `PublishRegionGuest`,
  `DrainRegionAuthority`; geometry, partial transfers, version/owner commit,
  complexity bounds and failure preservation.
- `texture/texture.cpp`: `GuestTexture::MappingsValid`, `SetupGuestMappings`
  callbacks, `SynchronizeHost`, `SynchronizeGuest`; residence admission,
  nonblocking CPU trap protocol and unlocked-metadata I/O.
- `texture_manager.cpp`: `FindOrCreate` collects non-view contained overlaps.
- `common/address_space.h/.inc`: `TranslateRange` / `TranslateRangeImpl` report
  residence under the translation lock; existing callers can omit the output.
- `interconnect/common/textures.cpp`, `interconnect/fermi_2d.cpp`,
  `interconnect/maxwell_3d/pipeline_state.cpp`: carry residence proof from all
  texture-producing translations.
- `tests/gpu_backing_promotion`: actual Vulkan bytes, failures, CPU protocols,
  writer changes, bounded footprints and compressed copies.
- `tests/gpu_region_residency`: exact production GMMU translation bodies.
- Existing small host facades in texture-sequence, sampled-layout and linear
  readback tests are kept compatible; assertions are preserved.

`buffer.cpp`, `buffer_manager.cpp` and `command_executor.cpp` were audited but
not modified. Their missing cross-resource/generation/transaction contracts
cannot be supplied safely by pretending an image copy is a generic buffer copy.

## Memory, performance and device acceptance

The common dense representation remains compact; clean synchronization does
not allocate version snapshots. The finer fallback adds per-GOB bindings and
partition metadata, plus temporary footprint/copy vectors. Existing full-size
staging can still be used to encode guest data, while the GPU repair copies
only selected regions. Readback on actual CPU access is still a whole-image
read into private staging followed by masked physical publication. This stage
does not implement arbitrary partial image downloads.

Guest padding remains retained. The 4096-GOB fallback budget limits metadata
per member (2 MiB of represented physical data), not total GPU memory and not
large dense texture support. Metadata work scales with involved component
members/regions, not the global managers. Device memory and performance effects
remain unmeasured. There is no claim of unchanged FighterZ or Zelda FPS.

Required phone acceptance: Moto G100 / Snapdragon 870 / Adreno 650, ARM64
DevRelease, identical driver/cache/settings. Compare FighterZ and Zelda launch,
menus/gameplay/frame times, repeated rendering and memory pressure, with
FreeGuest OFF/ON. Also validate supported 3D/layout workloads and driver copy
legality. Keep Draft; no merge. Existing PR Build/assembleDevRelease is used
without workflow changes or continuous Actions monitoring.

## Final host verification

All completed with exit 0 after the fixes:

- `TEXMAN_AUTHORITY_PROBE=1 python3 tests/gpu_backing_promotion/run.py`;
  repeated with `CXXFLAGS` adding `-fsanitize=address,undefined`,
  `ASAN_OPTIONS=detect_leaks=0`. Four original A→B→A combinations: 0/1024.
  Expanded patterned layout/3D, BC1, transfer-writer mutation, bounded-footprint,
  CPU trap/preparation/publication and failure controls pass with zero Vulkan
  validation errors.
- `TEXMAN_LOCK_ORDER_PROBE=1 python3 tests/gpu_backing_promotion/run.py`:
  PASS in both image-format-list modes.
- Full `python3 tests/gpu_backing_promotion/run.py`: all historical aliases,
  non-reversible writer rejection, deferred lookup failures, views, mip backing
  growth and format controls pass with zero Vulkan validation errors.
- `gpu_texture_sequence`, `gpu_submission`, `gpu_sampled_layout`,
  `gpu_storage_preflight`, `gpu_overlap_materialization`, `gpu_draw_texture`,
  `gpu_framebuffer_cache`, `gpu_pushbuffer_sequence`, `gpu_linear_readback`,
  `gpu_region_residency`, `gpu_texman_multirange`: each `tests/<name>/run.py`
  passes. Their configured UBSan/ASan checks remain enabled. Driver leak checking
  is disabled; no leak-check result is claimed.
- `git diff --check`; no workflow or shader-compiler/submodule changes.

No Android build completion or phone validation is claimed by these host tests.
