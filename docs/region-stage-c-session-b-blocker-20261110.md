# Session B — selective withdrawal release blocker

## Result and stopping point

Remote PR400 was confirmed at `2eefb90687d7ce114dc2cc3e8f0cc21399e43f6a`;
master was confirmed at `f0a95dd80eb150e81d16e1b0fa88c3a812a820c8` before any
edit. The integration checkout was clean. All five published checkpoint commits
are retained. The user reports no perceptible change/regression from its Moto
G100 APK; no new device claim is made here.

**No selective-admission/withdrawal protocol was enabled or implemented.**
`MixedRegionAuthorityEnabled` stays false. Session B stops at a focused,
content-verified architectural blocker, rather than adding flags that would
silently drop future coherence. Only the reproducer and this report change.
No push, APK request, benchmark rerun, broad review, agents or game investigation
was performed in this session. No SDK/NDK installation was attempted.

## Existing behavior traced

- `TextureManager::RegisterBufferRegions` and
  `BufferManager::RegisterTextureRegions` discover physical overlaps only behind
  the disabled production gate. A failed mixed registration throws; successful
  discovery stamps manager revisions. These stamps track discovery, not future
  data ownership of an excluded peer.
- `Texture::RegisterMixedRegionAuthority` expands old component membership,
  defers when a resource is attached, then prepares typed partitions and replaces
  groups only on success. Unsupported peers leave old group identities and
  versions intact. This is safe rejection, not a persistent excluded domain.
- `Texture::DrainRegionAuthority` publishes current owners, checks unpublished
  cells, then clears all members' groups, bindings, neighbors and mixed flags.
  It preserves current data; it has no ongoing component membership afterward.
- `Buffer::RecordRegionWrite` returns when the buffer has no regional group.
  Consequently it cannot advance a detached image's versions or invalidate it.
  `Texture::RequireAliasCoherence` cannot discover this later producer if that
  component is intentionally kept on the legacy/excluded path.

The missing step is therefore **future-use coherence after withdrawal**, not
merely flushing older GPU work or resetting a cached eligibility bit.

## Deterministic real-Vulkan boundary test

Added `CheckMixedWithdrawalBoundary` in
`tests/gpu_buffer_image_sequence/admission_tests.inc`, included by the existing
backing fixture. It uses actual production registration, drain, executor writer
publication and regional repair with real Vulkan backings and readback.

The production gate stays false even in the test. Initial mixed registration is
explicit. This deliberately models a component that would be excluded from
future automatic mixed rediscovery; it is **not** a completed implementation of
such a policy or a newly reproduced regression in normal checkpoint execution.
No production conditional or codec is modified.

Sequence:

1. Admit existing fully resident staged buffer and dense native RGBA8 sRGB image.
   Clear the image green, read the overlapping buffer and verify actual bytes.
2. Introduce a pitched RGBA8 peer, an existing materializable image path outside
   the validated mixed/reversible-authority domain. The production
   `IsGuestLayoutReversible` eligibility predicate rejects pitch layouts. Queue a real reader of the old buffer backing.
3. Attempt registration while the old consumer is attached. The actual function
   requests unwind/submission; group identities, versions, writer serial and
   pending node remain unchanged. Submit and verify the old reader's bytes.
4. Retry registration after submission. It rejects the incompatible peer without
   falsely attaching it or replacing the original component.
5. Compare retaining that ledger against actual `DrainRegionAuthority` followed
   by legacy/excluded use. In the latter, assert the drain published every current
   guest byte and removed associations as specified.
6. Write 0x6D through the retained buffer view on the GPU, then consume the retained
   image view. Check the shared 4096 bytes, image-exclusive bytes, buffer-exclusive
   bytes, unaffected guest guards and writer/resident version relationships.

| Policy exercised | Shared stale bytes | Image-exclusive mismatches | Old queued consumer |
|---|---:|---:|---|
| Reject peer, retain existing ledger | 0 / 4096 | 0 | Correct actual bytes |
| Publish/drain, keep component on legacy path | 4096 / 4096 | 0 | Correct actual bytes |

All eight cases (both format-list variants × Free Guest OFF/ON × the two policies)
match the table. In the failing policy, the buffer gets a newer real GPU writer
serial, the detached old ledger does not advance, and the stale image is not
invalidated. The observation persists after correct submission ordering.

Command:

```
source ../test-env.sh
ASAN_OPTIONS=detect_leaks=0 LP_NUM_THREADS=2 SANITIZE=address,undefined \
TEXMAN_MIXED_WITHDRAWAL_BOUNDARY=1 python3 -u tests/gpu_backing_promotion/run.py
```

Evidence retained at `../session-b-withdrawal-boundary.log`. Clang18 host fixture,
ASan+UBSan and Vulkan synchronization validation passed; zero validation errors.
The reproducer intentionally asserts the stale result for the drain policy: a
passing diagnostic is **not** a passing withdrawal acceptance test.
`git diff --check` also passes. No full historical regression or performance gate
was rerun because production/domain-cache code is unchanged.

## Why the small alternatives do not satisfy admission

- Ignore/reject the new association and let the incompatible consumer/writer
  proceed: it has no authority/invalidation relation to the already admitted
  resources. Existing safe rejection prevents that access, but is not the
  required ordinary excluded-resource behavior.
- Drain and mark the component excluded: demonstrated above to preserve old data
  yet lose subsequent producer/consumer coherence.
- Cache only component eligibility or manager revisions: changes no writer,
  resident version or callback relationship after a GPU write. It cannot repair
  the demonstrated missing future edge.

A correct replacement needs a persistent component/use relation that survives
transport exclusion, plus prepare/revalidate/commit transitions protecting all
retained views and pending consumers. Unsupported peers must either participate
through a proven existing transport or have a demonstrably safe disposition;
their physical overlap cannot be omitted. This crosses the current binding,
producer-publication, trap-preparation and resource-retention interfaces. It is
not established by a small registration predicate or drain change.

DMI remains especially separate: image publication can overwrite imported guest
memory while an old buffer consumer reads it, and this host fixture cannot prove
Android import/cache visibility. An authoritative image with no inverse codec
cannot be declared guest-published after a skipped download. Sparse and unproved
footprints likewise remain outside admission. No attempt was made to implement
these transports or broaden supported formats/layouts.

## Required next decision / minimum remaining proof

Before a session dedicated to experimental activation, establish a bounded
persistent excluded-component protocol using the existing staged transports,
with:

- observing every physical peer and retaining ownership/dependency relations even
  when that peer is excluded from the direct/bridge transport;
- transactional admission/withdrawal receipts for retained old views and pending
  reads/writes, with no metadata/trap lock held during waits;
- a proven disposition for incompatible arrivals, rather than untracked legacy
  reuse, and explicit limits for peers whose writes cannot be published;
- content tests that turn the drain/exclusion result above into 0 stale bytes,
  followed by map changes before/during preparation, concurrent publication,
  allocation failure, cancellation, rollback and retry.

Those new-protocol tests are **pending**, not reported as passed by this session.
The existing admitted domain and guest bridge remain the prior validated
checkpoint. Selective admission is not ready for an activated APK. No Session C
or further architecture work starts automatically.
