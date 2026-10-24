# Partial texture overlap authority

Depends on PR #392, commit `7c42375a`: successful-readback publication and the
Linear guest-copy direction remain intact. This change addresses the proven
partial-overlap defect; UE4 graphics and Android frame rate still require device
testing.

## Invariants

An overlap component records authority by guest CPU-address interval. A GPU
write claims only the actual encoded texel footprint, excluding padding. The
latest owner remains recorded after its readback becomes Clean; an older image
cannot regain authority merely because the newer image no longer needs download.

Readback first encodes into one reusable private buffer per component, then
publishes only intervals still owned by that image. It never temporarily writes
superseded bytes into guest memory. Guest validity is published only after the
copy completes. Completely superseded writers skip the copy without inventing a
readback serial.

Before consuming a stale image, resolve its pending intervals through their
current GPU owners, preserving each owner's exclusive GPU-only bytes. Upload the
composite guest contents lazily into the existing image. This permits A→B→A
without invalidating away A's exclusive data. Exact aliases in a partial
component consult the same ledger, including aliases introduced after B has
superseded A. Existing exact-only alias behavior is retained.

Mappings and footprints are distinct: dependencies include later fragmented
segments, while authority covers texels only. Mip growth resolves prospective
texels before initialization and then replaces the footprint after successful
promotion. Shared component pages are not passed to FreeGuest, because page
discard cannot preserve individual owners or padding.

## Ordering and recovery

GPU preparation and registration use the existing `channelLock` contract.
Registration/reconciliation requests unwind to the executor retry boundary,
submit queued work, and release its resource locks before resolving sources.
No additional global execution lock is introduced.

Registration takes sorted resource locks, state locks, and old component
operation locks. Reconciliation snapshots pending sources, then takes sorted
source/target resource locks and the component operation lock; it does not lock
unrelated component members. CPU trap callbacks try-lock their state, component
and resource locks under TrapManager's mutex. Contention returns false; blocking
preparation occurs outside that mutex. No cross-resource wait occurs inside a
trap callback.

Ledger boundaries and registration metadata are allocated before publishing new
authority. Encoding/allocation/recording failures retain pending authority and
retryable states. A failed reconstruction upload remains CpuDirty. A partial
component retains its trap handle when protection is removed, allowing rearming.
Native footprints are reconciled before late exact-alias registration takes its
resource locks, so failure cannot publish an invalid exact group.

No-overlap textures bypass the ledger and private encoding path. There are no
new GPU submissions, readbacks, component locks or global locks on that path.
Steady reuse of coherent overlapping images also avoids repeated submissions.
Unsupported lossy GPU writers fail explicitly rather than publishing fabricated
guest bytes.

## Local validation

- `tests/gpu_overlap_authority/run.py --check-correct`: real Vulkan images,
  commands/fences and synchronization validation, production manager,
  synchronization, TrapManager and IntervalMap; A→B→A, B Clean, exclusive bytes,
  fragmented mappings, layers/padding, mip growth, exact aliases, contention and
  failed readback/upload retry.
- `tests/gpu_guest_readback/run.py`: #392 publication/failure guards.
- `tests/gpu_sampled_layout/run.py`: layout/alias/copy regression checks.
- `tests/gpu_submission/run.py`: submission dependency ordering.
- `tests/gpu_backing_promotion/run.py`: existing alias/promotion regressions.

The host harness substitutes Android allocator/scheduler/lifetime boundaries.
It does not execute NCE's ARM64 signal handler or measure device performance.
A write-trap callback alone is not evidence of an actual CPU store; both a
read without store and a separate real store are tested.

Existing backing fixtures were adjusted to reflect #392's retained GpuDirty
state on unavailable encoding, the manager's eligible-first alias registration,
and the fact that discarded padding cannot be recovered from GPU texels. Unused
test command buffers are cancelled at cleanup rather than waited as submitted.
These changes do not alter production exact-only policy.
