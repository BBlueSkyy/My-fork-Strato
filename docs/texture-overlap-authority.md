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

## Follow-up: pre-registration sampled eviction

A sampled-only native image can be GpuDirty with `memoryFreed=true` and write
serial zero: generic attachment uploaded it and FreeGuest discarded its guest
pages before any overlap component existed. The initial #393 registration
ignored every zero-serial image. Unlike #392's FindOrCreate fallback readback,
it therefore let a new partially overlapping image upload discarded bytes.

Registration now retains an evicted backing as zero-serial recovery history.
Actual newer writers still win by serial; no write or readback serial is
fabricated. Reconstruction uses the existing masked publication/retry path.
The targeted test failed before this change and passes with real Vulkan and
`MADV_REMOVE`, including context/non-context creation, readback failure/retry,
exclusive bytes, later GPU writes and steady reuse without new submissions.
There is no added work for non-evicted images or the no-overlap path.

This establishes that specific regression when Free Guest Texture Memory is on.
The supplied SpongeBob video shows severe corruption but cannot establish the
option's state, resource mappings or which upload first consumed invalid bytes.
The correction's visual effect must still be tested on device; it does not
establish that all corruption or the lack of confirmed controllable gameplay
has been resolved.

## Integration follow-up: cached descriptors and HDR investigation

The log from 2026-10-24 identifies Turnip Adreno 650, Vulkan 1.3.280, driver
24.0.99 and BCn support. It contains Shader Compiler STUBBED warnings and
recoverable OUT_OF_POOL_MEMORY reports, without a shader hash, resource binding,
value dump or failing draw connecting them to the corrupted image.

Inspection across #392 and #393 found:

| Path | Finding |
| --- | --- |
| TIC/header cache → ordinary binding | GetTexture can return a cached view; GetTextureBinding/GetImageBinding still call AttachTexture, which checks alias/partial coherence. A cache hit alone is not a bypass. |
| Retained Vulkan descriptors | PrepareDraw can reuse the descriptor set without GetTextureBinding or AttachTexture, including unchanged bindings during partial quick bind. After B writes overlapping texels, cached A can remain the old VkImage despite overlapStale. |
| Cached color/depth state | Its refresh is also conditional; checking current attachments during preparation covers their reuse. |
| Descriptor allocation | AllocateSet handles pool exhaustion/fragmentation by resetting only unreferenced pools or allocating another pool, and returns a set only on success. The log alone does not demonstrate invalid descriptors. |
| Constant buffers/samplers | Their decoding and bindings are unchanged by #393. Buffer reads already have a submit/retry boundary for pending GPU writes. No value-level divergence for the HDR shaders was demonstrated. |
| Framebuffer/formats/layouts | Creation/view validation and framebuffer-cache code are unchanged. A correct layout/barrier cannot repair stale contents in a distinct overlapping image. No separate format/transition defect was established here. |
| Executor/scheduler | Submission dependency ordering is retained. The missing check concerned consumers that bypassed attachment preparation, before any fence or barrier could restore their backing. |
| Shader Compiler | No compiler/submodule change is introduced by #393 and no miscompile was demonstrated. STUBBED does not identify a causal instruction or shader. No compiler change is justified. |

A Vulkan fixture retains sampled A, writes green through partially overlapping B,
and confirms that A's VkImage still contains the old red bytes. The complete
production CreateRenderPassWithSubpass accepted this cached reference before the
fix (RED: no reconciliation request). Thus the integration of #393's partial
stale flag was incomplete, independently of Free Guest Texture Memory. The
similar exact-alias cache problem predates this change; this follow-up limits
new guards to tracked partial components.

PrepareDraw now checks actual color/depth attachments and retained sampled
bindings in the reuse branch, including partial quick bind. It uses the existing
reconciliation request. Checks occur before descriptor updates can claim storage
writes and before Draw converts quad indices; placing a throwing guard at the
later AddSubpass boundary would be unsafe. Coherent reuse takes no new GPU wait,
submission or lock. A stale old binding that a partial update would remove can
cause conservative reconciliation; no Android FPS claim is made.

The focused test compiles the exact preparation guard and complete production
render-pass method, with host facades for Android pipeline/state builders and
render-pass node bookkeeping. It verifies masked composite bytes using real
Vulkan readback, cached sampled/RT retry before nodes, and stable reuse without
extra submissions. It does not execute a complete Maxwell draw or shader sampling.

The prior observations for EA18B70958415FF3 (first abnormal HDR values) and
1B8CB95A440E18C1 (first NaNs) remain relevant consumers to check on device. The
current log does not map either hash to a stale A/B pair, sampler, constant-buffer
value, or recorded descriptor. Their prior SPIR-V/input binaries were not
available in this checkout, and searching those hashes did not recover them.
No OpUndef investigation was repeated. This proves a stale-consumer path, not
that it caused those shader values or all of the recorded visual regression.
