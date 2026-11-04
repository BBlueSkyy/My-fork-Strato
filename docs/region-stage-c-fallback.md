# Unsupported mixed overlap: fallback feasibility

Read-only source assessment, 2026-11-04, production HEAD
`d5e3dd7f2a9874cebf5e78f74f7021126c5b5d13`. No implementation or heavy tests
were performed. The only new file is this report. The production gate remains
false. This report does not establish that a fallback works or authorize activation.

## Finding

I3 is still a release blocker. There is no existing generic mixed fallback that
can safely replace the two manager exceptions. A staged, native, reversible
guest-bridge transport is feasible as a separate incremental authority mode, but
it needs persistent byte ownership and future-use invalidation. DMI and
authoritative non-reversible images need additional proofs and cannot be covered
by calling the current drain, alias registration, or readback helpers.

The smallest defensible next implementation is a disabled staged guest-bridge
increment. Preserve the current direct mixed domain and its transport unchanged;
do not broaden `PhysicalImageSpans` merely to force unsupported layouts through
the raw RGBA8 GOB copier. Global activation remains blocked until ordinary
unsupported encounters have a correct disposition, including a peer arriving
after a supported group already exists.

## What the current code actually guarantees

| Source | Relevant behavior |
|---|---|
| `texture_manager.cpp:59-70`, `buffer_manager.cpp:153-169` | Both manager directions discover physical overlaps and throw on failed mixed eligibility. Lookup revisions cache successful discovery only. |
| `texture/mixed_region_authority.inc:3-53` | Expands existing groups before eligibility; an attached member requests executor submission/retry. Then rejects exact-alias members, nonnative/non-UNORM images, mips/layers, edge GOBs, oversized span sets, and direct buffers. |
| `texture/region_authority.h` and `region_authority.inc:253-315` | Typed latest-writer intervals, guest versions and per-resource resident versions preserve independent producers. Image bindings also contain geometry required by the direct GPU repair path. |
| `buffer_regions.inc:100-141` | CPU preparation gathers only unpublished donors of the requested buffer, locks them outside metadata, revalidates, and publishes. This is a useful pattern, not a generic unsupported registration implementation. |
| `texture/region_authority.inc:542-595` | Drain publishes participating buffers and images, validates remaining unpublished owners, and detaches all mixed bindings. It leaves no future mixed producer/consumer relation. |
| `command_executor.cpp:1090-1111` | Retry submits old nodes before resolving registration/reconciliation. Unsupported mixed registration still throws after submission. |

Returning success and caching rejection alone loses coherence. After detachment,
`Buffer::RecordRegionWrite` has no group to invalidate, and
`Texture::RecordRegionWrite` is no longer called through a mixed relation.
`RequireRegionCoherence`/`RequireAliasCoherence` cannot find a donor from a
rejection bit. Conversely, leaving `regionStale=true` without bindings simply
creates repeated reconciliation or calls the existing unsupported discovery
again.

An image alias group is not a substitute. `RegisterAliases` requires matching
physical mapping sequences and proven reversible layouts
(`texture.cpp:942-961`); its owner and invalidation membership are images only.
Read-only materialization prevents an unencodable image becoming a writer, but
does not make a later buffer write invalidate that sampled image. Retirement
guards retained texture views; it does not replace mixed authority for live
buffers or arbitrary partial overlaps.

## CPU mirror and publication boundary

Buffer and texture mirrors alias actual guest memory. Writes through those
mirrors are publication, not a private scratch update. They must not be assumed
to trigger the original guest-address trap callbacks.

Regional buffer readback copies only owned unpublished cells
(`buffer.cpp:531-555`). Regional image readback encodes privately and copies only
owned cells (`texture.cpp:579-622`, `region_authority.inc:278-286`). A nonregional
buffer instead publishes its whole backing; a nonregional image encodes directly
into its guest mirror. These legacy whole-resource paths are unsafe for
independently stale, partially overlapping producers unless future access is
serialized and each source has first been made coherent.

An image encoder writes pixels/format blocks, not the entire mapped allocation.
The block-linear encoder skips unused columns, edge-sector tails and Y/Z
padding (`texture/layout.cpp:185-282`). `FreeGuest` is separately gated by proven
complete coverage and by regional membership (`texture.cpp:625-635`). A fallback
cannot treat a zero-initialized encoded allocation, a mapping extent, or a whole
edge GOB as image-owned bytes. Doing so destroys padding or a buffer's exclusive
bytes. Copying original guest bytes into a temporary encoding is insufficient
unless ownership/version validation still masks foreign writes at commit.

Trap preparation runs outside `trapMutex`; read/write callbacks run under that
mutex and must only try-lock and validate (`common/trap_manager.cpp:120-181`).
An image's current preparation publishes its own image state, whereas a buffer's
regional preparation gathers foreign donors. A guest-bridge image can also have
foreign buffer donors: both image and buffer traps therefore need the same
donor preparation rule. Returning early because an image is Clean is not enough
when another resource owns its bytes. No GPU wait, resource acquisition, or
readback submission may be added under `trapMutex` or regional metadata.

## Smallest staged reversible increment

The following is a concrete design boundary to implement and validate, not a
claim of completed correctness.

1. **Add a persistent guest-bridge mode to authority.** Retain typed interval
   writers, `hostVersion`/`guestVersion`, resource resident versions, weak
   membership and invalidation. Add a transport discriminator and guest byte
   bindings independent of `PhysicalImageBinding`'s direct-copy geometry. Both
   manager directions must install this relation, or explicitly reject a hard
   unsupported boundary before an operation publishes a node. Cache eligibility
   with the relevant manager revisions, mapping identity and resource layout/
   backing generation; a rejection cache never removes the persistent relation.
   Existing component closure is structural work, not a per-operation global
   resource scan.

2. **Prove native encoder coverage.** Initially admit staged buffers and native
   optimal, single-sample, reversible 2D block-linear color images only, using
   `IsGuestLayoutReversible` plus native-format/backing checks. sRGB, mips/layers
   and edge GOBs can be candidates for this transport without entering the raw
   GOB domain. Derive exact contiguous byte runs from the existing encoder's
   address calculation, including mip/layer offsets and physical mapping
   boundaries; test that the run set exactly matches encoder writes. A byte
   binding must never include padding. Establish and document a setup metadata
   bound and checked size arithmetic. Exceeding that bound is an explicit
   unsupported boundary, not permission to ignore a live peer. Coverage of all
   normal oversized layouts is a later requirement if global activation exposes
   them.

3. **Join transactionally.** Expand involved existing components, retain the
   resources, prepare coverage/partition allocations, then acquire resource,
   state and metadata locks in the established order. Attached producers **or
   consumers** require unwind/submission/retry before changing their prepared
   snapshots. Preserve old interval histories, guest versions and pending
   dependencies when splitting cells. A newly created CpuDirty representation
   is not a GPU producer. If a newcomer has prior authoritative writes, resolve
   its exact covered bytes against old interval versions; do not blindly inherit
   the old group or replay whole images in lookup order. Publish the new graph
   only after every preparation succeeds. Failure retains the old graph,
   delegates, byte owners and resource lifetimes. Draining once and deleting the
   graph is not this transition.

4. **Repair through canonical guest bytes on each stale use.** Snapshot only
   latest donors intersecting the target's complete upload footprint; retain,
   acquire and revalidate them as `Buffer::PublishRegionGuest` does. Submit a
   necessary pending executor use first. Publish donors with owner/version
   masking, outside metadata/trap locks. Include the target's own unpublished
   exclusive bytes before any full target upload, otherwise marking it CpuDirty
   would discard them. Once every byte needed by that upload is guest-current,
   upload through existing `SynchronizeHost(false)` and record the captured
   resident versions. Revalidate and retry on changed ownership. Do not refresh
   a resource with pending older consumers in place. Wait only for actual
   donors and the target's prior accesses; no device idle or unrelated-component
   readback. Full private staging/readback for a necessary image is acceptable;
   guest publication remains masked.

5. **Keep future writes in the ledger.** Reuse `PrepareGpuWrite`,
   `PrepareRegionGpuWrite`, `AcquireRegionPublication`, retained node commit and
   `RecordRegionWrite`/peer invalidation. Dispatch stale reconciliation by
   transport mode before current raw GOB assumptions. The image encoder publishes
   only byte bindings owned at the validated version. CPU callbacks relinquish
   only their own/guest-owned cells after donors are current. Guest-bridge
   membership must inhibit FreeGuest and readback-skipping hacks just as regional
   membership does now. Coalesce, promotion and descriptor reuse must preserve/
   migrate this mode, coverage and versions, or perform a transactional supported
   transition; an old lookup-revision stamp is not sufficient.

6. **Cover every use path.** `RequireAliasCoherence`,
   `RequireRegionCoherence`, cached descriptors, Buffer Read/Write/Copy,
   `GetReadOnlyBackingSpan`, megabuffering and mapped CPU fast paths must honor
   the mode. `TryMappedWrite`/`TryMappedCopy` currently bypass regional writes
   only when `regionTracked` is false: a separate fallback bit without updating
   these guards would escape publication. `TrackRegionUse` and
   `AcquireRegionPublication` also currently filter on `regionTracked`. Retain
   the current simultaneous-conflicting-writer rejection before node publication.

This can be implemented incrementally behind the disabled release gate and a
temporary test activation. It is a new transport/lifetime proof across existing
hooks, not a two-manager exception-handling change. A fallback mode must be
implemented before suppressing those exceptions.

## Hard boundaries that the staged increment does not solve

**Non-reversible authoritative images.** `SynchronizeGuest` returns without
encoding for a converted host format (`texture.cpp:1494-1497`) and read-only
materialized aliases return immediately. A skipped readback is not a successful
publication. Decoded compressed data has no lossless inverse in this code.
Retirement is only valid after proven publication or a strictly newer complete
coverage overwrite. Partial overlap and a newer whole-image serial do not prove
that all exclusive bytes were superseded. Unknown/depth/MSAA/3D layouts require
their own codec/coverage proof. Sampling-only materialization is possible only
with a persistent mixed invalidation relation and a preserved authoritative
source; it cannot admit writes through the unencodable image.

**DMI pending accesses.** `SetupStagedTraps` skips direct buffers, and
`Buffer::SynchronizeHost`/`SynchronizeGuest` do not perform staged transport for
them (`buffer.cpp:25-26,493-495,531-533`). Their guest mirror is imported GPU
backing. Image publication into guest memory can overwrite an imported buffer
while the GPU is still **reading** it, even when that buffer is not the latest
writer. `RefreshGpuWritesActiveDirect` covers writes; it does not authorize this
external CPU overwrite. `RefreshGpuReadsActiveDirect`, tracked shadow writes,
`BackingImmutability`, `RequiresCycleAttach` and `UpdateCycle` are per-buffer
mechanisms, not cross-image publication leases. A DMI extension needs all
intersecting imported access receipts, current-executor consumer submission,
proper completion/visibility before CPU publication, shadow handling and future
invalidation. Treating isDirect as a staged flag or polling writes only is
incorrect. The host admission-flag fixture cannot validate Android imported
memory or cache visibility. Keep this a hard gate until that proof exists.

These boundaries mean a staged reversible fallback alone does not make the
single production switch safe to enable globally. Per-component activation also
needs the arrival/retirement protocol above; a future unsupported peer defeats
one-time admission tests.

## Required acceptance evidence

- Real manager-driven B -> unsupported native image -> B byte oracles for sRGB,
  edge width/height, mip/layer, padding/stride gaps and a larger-than-direct-domain
  image. Continue alternating writes after first fallback; ordinary tested
  reversible overlaps must not throw.
- Start with an existing supported mixed group and pending buffer/image writers
  and readers; add each unsupported reversible peer. Verify future writes through
  all retained old views, not just the first drain. Exercise reverse discovery,
  secondary mappings, cached descriptors and rejection-cache invalidation.
- Partial buffer write crossing an edge GOB; image and buffer exclusive bytes;
  already-published foreign guest bytes; newest writer ordering and a CPU write
  during publication. Assert all guest bytes, target backings and versions,
  including every padding/tail byte.
- CPU read/write faults through both image and buffer registrations with foreign
  donors; no wait/readback under trapMutex/metadata; fast readback hacks and Free
  Guest OFF/ON. Delay a disjoint component producer and assert it is not waited
  or downloaded.
- Coalesce while attached, promotion, remap/retry and retirement with pending
  consumers; injected allocation/retention/recording failures leave the old
  authority/lifetimes valid and replay the complete logical operation once.
- Negative controls must demonstrate stale bytes when persistent invalidation,
  coverage masking or consumer-submission protection is removed. Admission-only
  rejection tests do not satisfy these cases.
- DMI and non-reversible cases must remain explicit negative boundaries with no
  fake publication, partial graph detach or writer/node claim. Actual DMI support
  additionally requires imported-memory producer **and reader** tests on the
  relevant Android/device path.
- After implementation, rerun the existing integral producer/consumer, executor,
  mapping, coalesce, sanitizers and Vulkan synchronization-validation gates,
  then assess stable supported-path allocation/copy/wait regressions. Existing
  green logs do not test this proposed transport.

Recommendation: preserve the validated disabled implementation and I3 blocker;
implement and prove the staged reversible guest bridge as a bounded next
increment. Do not describe drain/rejection coverage as fallback completion, and
do not enable production until remaining hard-boundary encounters have a proven
coherent disposition.
