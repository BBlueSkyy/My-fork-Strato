# Safe texture eviction: implemented subset and remaining boundaries

# Guest recovery / texture ownership reference audit

Base: Strato `7c42375a86dd1d8e7694505d55a4bf4c4a0951cb` (#392). This branch imports no production changes from #393 or #394. The source comparison below is not evidence of the cause of UE4 corruption on device.

## Finding and recommendation

A conservative dependency graph is viable for the subset whose GPU contents can be losslessly encoded by the current `CopyToGuest` path. It does not require an interval ownership ledger. Its invariant must be **no two independently authoritative GPU images overlap in guest bytes**. Before transferring ownership to image B, flush every authoritative conflicting image A in full, preserve A-only bytes in guest memory, invalidate A's GPU representation, and refresh B from the resulting guest bytes **before** recording any partial GPU write into B. Later A use rebuilds lazily. A stale image must never flush afterward.

This is a concrete generalization beyond exact mapping aliases, but it is not a general solution for unsupported readback conversions, allocation lifetime reuse, unmapped memory, or sampled / storage / rendering through mutually overlapping independent images in the same operation. Keep these as explicit boundaries.

### Proven gap at this baseline

`TextureManager::FindOrCreate` accumulates nonrepresentable partial overlaps in `matches` and only calls `SynchronizeGuest(false, true)` on them when materializing a new texture (lines 521–533). It does not register a lasting relationship for those partial overlaps. Reusing `fullMatch`, `layerMipMatch`, or `depthSliceMatch` returns earlier. `RecordGpuWrite` (1139–1163) invalidates peers only in an existing exact-mapping `AliasGroup`.

Consequently the source permits this deterministic stale-cache sequence, independent of visual color:

1. Create A, render A, then create separate, nonrepresentable partially overlapping B.
2. B creation reads A back; A becomes Clean. B uploads the shared bytes.
3. Render B. Neither A nor B belongs to a shared exact-mapping group.
4. Reacquire cached A. `FindOrCreate` returns its existing view, `RequireAliasCoherence` sees no stale marker, and `SynchronizeHost` sees Clean; A's GPU image retains the old shared pixels.

A second hazard is the full-copy converse: after B owns newer overlapping bytes, blindly reading A back as a whole image can overwrite them. A timestamp alone does not preserve A-only bytes. Draining A **before** B writes is what makes whole-image copies safe.

## Concrete references fetched

These are actual source downloads through GitHub file tools, not search-result descriptions. Eden is the project's GitHub mirror. Ryujinx is a third-party source mirror; all conclusions below are about the pinned source, not a claim that this is a currently authoritative upstream release.

| Code family | Pinned source | Functions inspected |
| --- | --- | --- |
| Ryujinx dependencies | https://github.com/Stella-sea/ryujinx-admin/blob/fd88d4ce7d049c2f6536439f0750b14f306d24b7/src/Ryujinx.Graphics.Gpu/Image/TextureGroupHandle.cs | `SignalModified`, `SignalModifying`, `DeferCopy`, `CreateCopyDependency`, `Copy`, `DiscardData` |
| Ryujinx partial ownership | https://github.com/Stella-sea/ryujinx-admin/blob/fd88d4ce7d049c2f6536439f0750b14f306d24b7/src/Ryujinx.Graphics.Gpu/Image/TextureGroup.cs | `InitializeOverlaps`, `CreateCopyDependency`, `RegisterIncompatibleOverlap`, `ClearIncompatibleOverlaps`, `ClearModified`, `FlushModified`, `SynchronizeMemory`, `FlushIncompatibleOverlapsIfNeeded` |
| Ryujinx mapping lifecycle / discard | https://github.com/Stella-sea/ryujinx-admin/blob/fd88d4ce7d049c2f6536439f0750b14f306d24b7/src/Ryujinx.Graphics.Gpu/Image/TextureCache.cs | `MemoryUnmappedHandler`, `ShouldDiscard`, `DiscardIfNeeded` |
| Ryujinx texture lifecycle | https://github.com/Stella-sea/ryujinx-admin/blob/fd88d4ce7d049c2f6536439f0750b14f306d24b7/src/Ryujinx.Graphics.Gpu/Image/Texture.cs | `Unmapped`, `UpdatePoolMappings` |
| Eden / inherited Yuzu texture cache | https://github.com/eden-emulator/mirror/blob/989768aa46720e3fcab9e2111e3ddd70819c382b/src/video_core/texture_cache/texture_cache.h | `WriteMemory`, `DownloadMemory`, `UnmapMemory`, `UnmapGPUMemory`, `FindImage`, `JoinImages`, `MarkModification`, `SynchronizeAliases`, `PrepareImage` |
| Eden image state | https://github.com/eden-emulator/mirror/blob/989768aa46720e3fcab9e2111e3ddd70819c382b/src/video_core/texture_cache/image_base.h | `ImageFlagBits`, `ImageBase::modification_tick`, alias and overlap collections |

## What the reference implementations actually do

### Ryujinx

- View-compatible storage shares groups; other copy-compatible regions establish persistent dependencies at matching layer/level ranges. `CreateCopyDependency(TextureGroup other, bool copyTo)` checks physical slice origin, `ViewLayoutCompatible`, and `CopySizeMatches`, then forces subdivision before linking handles (TextureGroup around 1550–1582).
- A GPU modification signals dependents. `DeferCopy` clears the destination handle's Modified flag, records a source handle, and makes all affected views dirty. The destination cannot later flush its superseded contents.
- `Copy` preserves the deferred copy while the source is actively bound (`_bindCount > 0`), so a single version/tick comparison cannot stand in for continued modification tracking.
- Incompatible overlaps have lasting bidirectional registration. Some host-incompatible formats trigger tracked guest readback in `FlushIncompatibleOverlapsIfNeeded` before upload. `ClearIncompatibleOverlaps` and `ClearModified` suppress obsolete overlapping ownership. Their modified bookkeeping is per subdivided handle/slice; copying these clears onto Strato's whole-image DirtyState would be unsafe.
- `FlushModified` flushes only modified slice ranges where possible. The reference architecture has finer ownership than Strato's one state per image.
- Discard is conditional: `ShouldDiscard` requires one depth/layer and a size hint covering the texture. The discard flag is not proof that an arbitrary draw writes every byte.
- `MemoryUnmappedHandler` identifies the old physical ranges before remap, invokes `Texture.Unmapped`, and schedules pool mapping updates after remap. `Texture.Unmapped` marks mapping changes and clears obsolete modified ownership. This is lifecycle handling, not a texture-format comparison.

### Eden / inherited Yuzu

- `PrepareImage` takes separate `is_modification` and `invalidate` flags. Normal use refreshes CPU content and synchronizes aliases; actual GPU modification advances `modification_tick`. A genuinely invalidating use skips old content. This is not evidence that sampled attachment alone should own GPU output.
- `SynchronizeAliases` selects newer aliases and applies their precomputed image-copy regions oldest-to-newest. `JoinImages` likewise sorts source copies by modification tick and retains overlapping-image relationships. These ticks work with copy-region information; simply sorting whole-image readbacks does not preserve independent partial histories.
- `DownloadMemory` sorts images by modification tick and waits for actual completion through runtime Finish before swizzling guest bytes.
- CPU `WriteMemory` marks every overlapping image CpuModified and untracks it.
- CPU physical `UnmapMemory` untracks/unregisters/deletes all affected images. GPU VA `UnmapGPUMemory` instead marks affected images CpuModified and Remapped; lookup explicitly excludes Remapped images. Neither is equivalent to flushing every old GPU image into a new mapping.
- The source itself has guarded and unimplemented cases for unsafe GPU copies. Treat this as support for capability gating, not proof every overlap can be made reversible.

## Conditions needed for a minimal Strato graph

1. **Edges cover physical ordered guest spans, including secondary mappings.** Reuse the overlap index; deduplicate images. A later lookup selecting an existing backing still needs registration. Restricting registration to new image construction leaves the demonstrated cache gap. Exact aliases should retain their existing specialized groups and reversible/read-only checks.
2. **Drain-before-invalidate-before-refresh-before-write.** Before B writes, A's entire losslessly represented GPU payload must become guest-valid. Then A loses GPU authority; B refreshes before the write. If B was already GPU-authoritative and disjoint C remains authoritative, preserve their independence. A single writer field for an arbitrary connected component is invalid unless the implementation drains the entire component before every ownership change.
3. **Read-use also checks dependencies.** B can be stale only from a write through A; cached descriptors and already-resolved views need the same reconciliation trigger as newly created views. `Texture::GetView` currently checks `RequireAliasCoherence`; a graph feature must also cover execution paths that retain a view without obtaining another one.
4. **Submit and retry outside resource locks.** `RecordGpuWrite` runs under render-pass setup / image usage tracking, often while the executor already owns resources. It cannot synchronously wait for another texture's unsubmitted current cycle. Existing `TextureAliasReconciliationRequest` and `CommandExecutor::HandleTextureBackingPromotion` provide the required Submit boundary. The new preparation must happen before the write marker mutates authority, and must unwind all locks first.
5. **Multiple writers in one operation need validation.** Distinct independent overlapping render-target/storage writers have no defined whole-image order. Existing `ValidateAliasWrites` catches only shared exact groups. Expand validation for graph overlaps, or prove compatible shared storage.
6. **CPU traps preserve outside bytes.** Before granting an arbitrary CPU write, flush all authoritative overlapping GPU data, because the CPU fault supplies no complete overwrite guarantee. Mark every affected cached image CPU-dirty afterward. Read traps must not grant access while relevant guest data is evicted or still GPU-owned. Keep the current try-lock/retry rule under trapMutex.
7. **Lifetime and exception safety.** Hold strong writer references or complete preservation during destruction; remove expired edges. Failed download, encode, upload, or submission must leave the old owner authoritative and guest invalid. New graph membership cannot publish stale/clean success prematurely.

## FreeGuest boundary

At the #392 baseline, `FreeGuest` (593–604) invokes `MemoryManager::FreeMemory(mirror)`; the latter rounds **inward** to full pages and performs MADV_REMOVE (memory.cpp 828–843). It removes guest bytes, rather than merely unmapping an expendable CPU view. `memoryFreed` is per texture, while the removed bytes may be shared by many representations.

A dependency graph may rely on guest bytes for lazy reconstruction only if those bytes remain valid. The safest narrow policy is to skip FreeGuest for members of a general-overlap dependency relation while retaining the user's setting for ordinary textures. When joining an already evicted member, first restore its authoritative reversible representation at a submitted boundary. Existing exact alias `RegisterAliases` already has specific restoration logic and guest leases; retain it.

Do not overstate the page issue: disjoint byte mappings sharing only a boundary page are not removed by inward rounding. The harder issue is logical texel coverage versus the full discarded span. `CopyToGuest` writes encoded logical texels/mips, while stride/alignment/padding bytes can lie inside full removed pages. An image containing only one logical subregion cannot restore bytes needed exclusively by another interpretation. Mapping equality alone is not proof of full byte coverage.

Skipping future FreeGuest cannot recover bytes already discarded without a reconstructible owner. Unsupported/nonlossless owners must fail closed or retain guest bytes from before eviction. Do not globally disable FreeGuest or enable a readback hack as a correctness fix.

## Unmap/remap is a separate boundary

At this base, `AsGpu::FreeMappingLocked` / `MapBufferEx` / `Remap` update GMMU spans without texture-cache notification; generic `FlatMemoryManager::Map/Unmap` changes tables. `MemoryManager::UnmapMemory` also frees pages. A texture dependency graph keyed to current physical pointers has no concept of allocation lifetime or mapped generation.

Do not infer that every GPU-VA remap corrupts data: newly translated physical spans can correctly select a different image, and a GPU VA alias remap is not itself destruction of the old physical allocation. But do not claim graph coherence also solves physical unmap/reuse: old texture destruction/readback can outlive the allocation's original meaning. Minimal remap invalidation needs an explicit lifecycle contract specifying whether to preserve old physical allocation contents, suppress obsolete readback, invalidate descriptors, and retain in-flight backing objects. Importing Eden's deletion or Ryujinx's clearing in isolation cannot supply that contract.

## Actionable scope

Recommended first implementable fix: persistent conservative overlap dependencies, stale-view reconciliation via existing Submit/retry, actual writer-only ownership, readback capability gates, and graph-local FreeGuest suppression/restoration. Retain exact-alias logic. Demonstrate byte preservation for A-only/shared/B-only data across alternating writes, CPU reads/writes, stale destruction, and cached view reuse.

Do not advertise general guest-memory coherence until physical allocation lifetime invalidation and unsupported reverse-format cases are addressed. No visual image or color result proves these invariants; assert byte patterns, authority states, readback ordering, and submission progress in focused tests.


## Implemented discard boundary

`MemoryManager::FreeMemory` keeps its guest-address contract. The new
`FreeHostMemory` entry point accepts a host mirror directly, avoiding a second
`guestOffset` translation. It returns success only after a nonempty
`MADV_REMOVE` succeeds; errors/no full pages do not publish successful eviction.
The existing page rounding remains inward, so partial boundary pages are kept.

`FreeGuest` retains layouts whose discarded byte span cannot be reconstructed
exactly by the production encoder. This includes row/GOB/mip/layer/tail padding
and unproven packed depth/stencil representations. Tight native color layouts
and validated single-plane depth formats retain eviction. Already-evicted data
is not declared recovered merely because a later free attempt would fail.

The production discard methods are exercised against shared Linux memfd pages,
including nonidentity guest translation, partial boundary pages, unsupported
private mappings and no-op ranges. The production encoder/discard regression
uses sentinel padding and real MADV_REMOVE; the baseline destroys padding while
the guarded path retains it. This changes memory-saving eligibility and can
retain more RAM for padded textures; it is not a game-specific rendering fix.

The #394 NaN/cubemap evidence does not establish an eviction fault: guest and
Vulkan agreement only locates those invalid values before upload. The above
changes are justified by memory roundtrip failures, not by those visual results.

## Deliberately not shipped: partial-overlap graph

A separate experiment reused the deterministic Vulkan A/B fixtures. The #392
baseline failed the stale-A byte assertion; a reversible RT-only eager bridge
passed cached-A / Clean-B / exclusive-byte checks with zero validation errors.
That result is insufficient to ship a general dependency mechanism.

Graphics and compute descriptor preparation currently interleave resource
resolution with storage writer claims. A draw can claim A, encounter partially
overlapping B and request Submit before any real draw/dispatch has been queued;
retry can claim A again and repeat indefinitely. A late throwing guard also
comes after quad-index conversion in Draw, so retries can transform its inputs
twice. Merely copying Ryujinx's dependent-dirty propagation onto this preparation
order would introduce new regressions.

A complete adaptation needs a consumer-preflight boundary that resolves and
validates every relevant texture binding before any storage/RT ownership claim
or irreversible draw-parameter mutation. It must cover full/partial descriptor
reuse, graphics and compute, CPU traps, exact aliases and allocation lifetime.
Adding only RT support and rejecting storage accesses would change game
compatibility without sufficient evidence, so neither policy is included here.

Consequently this branch does **not** fix stale partially overlapping backings,
A→B→A authority, physical unmap/reuse, or every possible Free Guest Texture Memory
failure. Those remain baseline limitations. No whole-image invalidation, range
ledger, dependency graph or speculative synchronization policy from the
experiment is part of this PR. The shipped changes only prevent demonstrated
unrecoverable page discard and incorrect mirror-address translation.

## Essential validation and costs

- Production discard API against memfd: nonzero guestOffset, host mirror,
  inward page rounding, no-op spans and failed MADV_REMOVE.
- Exact production CopyToGuest/FreeGuest/discard roundtrip: Linear/Pitch/Block,
  native packed layouts/mips, row/GOB/mip/layer padding sentinels, repeated guest
  mappings and packed depth/stencil exclusion, native D32 eligibility.
- Existing layout/alias/copy suite under ASan/UBSan and #392 guest-readback suite
  under UBSan. No extra CI tests or Android build configuration.

No GPU commands, readbacks, locks or per-draw cache scans were added. Eligibility
checks run only when eviction is requested. They deliberately retain more RAM
for layouts not proven reconstructible. Android FPS/memory measurements and UE4
visual correctness were not established. The workflow remains ARM64 DevRelease;
its base-branch filter permits this stacked PR. No RelDebug or Actions monitoring.
