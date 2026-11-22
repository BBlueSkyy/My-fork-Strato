# Cosmic Shake — Free Guest memory authority, 22 November 2026

## Result

Base: current master `8bcaee0be950b44234cdd2767a11578c74db2602`.

**The complete reason Free Guest ON improves the menu is still unproven.** The
experiments separate the setting's effect from generic gpuDirty acceptance; the
sampled logs do not identify the first incorrect texture or consumer. No claim
that the new candidate fixes SpongeBob visually, or that its FPS/frametime is
unchanged, can be made without device validation.

A concrete CPU/GPU coherence race remains in master: for an ungrouped texture,
SynchronizeGuest publishes Clean/CpuDirty and clears memoryFreed **before** the
GPU wait and guest encoding complete. Actual production trap callbacks can then
release CPU access to old/removed guest bytes. A deterministic gated-copy test
reproduces this in all eight read/write × resident/freed × readback-hack cases.
The selective publication correction from the earlier #392 investigation is
adapted to this master; it blocks all eight premature grants and preserves
failure/retry ownership. It does not alter FreeGuest's global policy or generic
gpuDirty policy.

PRs/branches #400, #460, #476, #477, #478 and #479 remain untouched. No merge.

## Evidence reused, not restarted

- #477 `d33059c909c88668d188cff66df0fc08dee18a2d`: bounded GPU-MEMAB categories on
  master, no shader/texel correction. #478 `bd4fb10be2251d0869235db36d43f6c49ff6bf6b`
  forces generic policy ON with Free Guest OFF; #479
  `9d9dfa0a13c180120b75f50750fd60e85f7e4ad4` forces it OFF with Free Guest ON.
  Their renderer differences were inspected; they are not production policies.
- Device observations supplied for this continuation: #478 OFF/ON-policy had
  corruption in 4/6 boots; #479 ON/OFF-policy had a consistent menu in the tested
  boots. No #479 log is included in the attached ZIP; its graphical result is
  the user's observation, not a new inferred counter result.
- #476's numeric texel-buffer correction did not resolve the visual corruption.
  No shader/texel investigation is repeated here.
- #389 already tested logical eviction while retaining physical pages; #390's
  source documentation records the user's return of corruption in that old
  variant. This makes physical discard/content effects relevant, but is not a
  controlled modern multi-boot result. The zero-fill experiment is not repeated,
  and no undocumented outcome is assigned to it.
- #392 / `fix/texture-guest-readback-publication` `7c42375a` previously established
  transactional readback and trap failure handling. Those changes are absent
  from the current master in the ungrouped path. Reuse its source/tests rather
  than reinventing a new synchronization mechanism.
- #395 `ad1ed036` already established unrecoverable padding discard and the
  host-mirror/guest-address API distinction on its own baseline. Its source and
  reports were read; it is not silently ported or advertised as a game fix.
- The known CheckFreedWriterGuestBridge failure was previously recorded during
  #459. It still fails on this base. Do not erase the test or hide it as a new
  regression of the publication correction.

## Four attached logs

Input: Logs_SpongeBob_PR477_PR478.zip. Input SHA256s and parsed records are retained
in `docs/checkpoints/cosmic-freeguest-20261122/device-events.json`. Pointer values
are not matched across boots. Counts below are **last sampled counts / lower
bounds**, not exact totals, rates or numbers of affected textures.

| Input | Last writer sample | Last readback sample | Generic policy sample | Other relevant samples |
| --- | ---: | ---: | --- | --- |
| #477 OFF, severe |32768|512|suppressed4096|free-guarded512|
| #477 OFF, almost correct |65536|512|suppressed8192|free-guarded512|
| #477 ON, almost correct |32768|1024|accepted2048|free-applied1024; freed-readback1024; one writer-restore attempt|
| #478 OFF/policy ON, corrupt |16384|1024|accepted1024|free-guarded1024|

Log durations and histories differ; readback counts alone do not distinguish good and bad output. Both OFF
#477 logs show the same categories and no sampled alias restoration despite
opposite visual severity.

No alias-implicit-rejected(-freed), alias-writer-restore-failed,
guest-readback-unavailable or alias-stale-download event occurs in these four
files. Because each category logs its first occurrences, there is no positive
record of those paths here. This does not exclude unsampled resource identity
or an earlier/later session. The ON restore attempt is a 128×128 native FP16
resource (Vulkan format 97), not proof it supplied the corrupt menu pixels.

**Instrumentation correction:** free-applied means FreeMemory was invoked and
memoryFreed was set. FreeMemory returns void; it can have no interior full pages
or fail madvise. Thus the event is not proof of successful physical eviction,
and the flag does not identify which shared pages remain valid. The logs do not
include actual discarded ranges, return status or pre/post byte contents.

## ON/OFF semantic difference and authority audit

| Mechanism | Free Guest OFF | Free Guest ON |
| --- | --- | --- |
| Generic attachment with no explicit writer | Normally suppresses gpuDirty | Normally accepts gpuDirty |
| Explicit RT/storage writer | RecordGpuWrite and serial tracking remain active | Same explicit writer mechanism |
| FreeGuest eligible native image | Retains guest physical contents | Calls FreeMemory on the mirror and marks memoryFreed |
| CPU read/write through a GPU-dirty trap | Should recover the current writer before granting access | Same obligation; removed bytes additionally need recovery |
| Alias eligibility | Requires resident guest bridge and reversible layouts | memoryFreed blocks eligibility until recovery; implicit path can reject too early |

The #478/#479 split shows generic acceptance is insufficient/unnecessary for the
observed consistency; it does not identify the responsible page or establish
that ON is semantically correct. Zeroing stale/undefined bytes, guest restoration,
page residency, timing and partial-overlap authority remain distinguishable
possibilities, not interchangeable explanations.

RecordGpuWrite records a write serial before submission; grouped writers also
advance generation, become GPU-dirty and invalidate peer aliases. Current
SynchronizeHost/Inline retain the policy distinction and explicit-writer
handling. ReconcileAliases checks the observed generation and guestGeneration
again before making a stale member CPU-dirty for lazy upload. No unconditional
alias revival or readback ordering change is added.

The current executor assigns a texture's cycle during SubmitInternal. A serial
alone does not prove a queued write was submitted. TextureManager's fallback
readbacks and cached partial overlaps therefore require separate investigation
when implicated by a concrete timeline. pendingGpuWriteTag is absent here; #460
was read as reference, not transported automatically. The candidate does not
claim to solve pending unsubmitted writers or general partial-overlap authority.

## Deterministic publication defect and correction

| Point in a gated readback | Original ungrouped texture | Corrected texture |
| --- | --- | --- |
| Before the guest copy | Clean/CpuDirty, memoryFreed=false, old readback serial | GpuDirty and original eviction state retained |
| Concurrent production read/write trap | Grants access; test guest byte still 7 instead of 42 | Nonblocking refusal; retry waits outside trapMutex |
| Successful fence and copy | Serial eventually becomes current | State, eviction flag and serial publish only now |
| Failed allocation/wait/record/submit/copy | Can leave false guest validity | Writer remains authoritative; retry succeeds |

Only `Texture::SynchronizeGuest` and its read/write trap callbacks change in
production. The method retains stateMutex while the existing wait/copy runs.
TrapManager::HandleTrap releases trapMutex before invoking its blocking lock
callback, avoiding a new global trap wait while a resource lock is held.
No new GPU copy, submission, barrier, device-wide wait or per-draw scan is added.

An actual unsupported/undefined GPU writer stays invalid and cannot silently
release a CPU trap. The existing valid compatibility case remains available:
a converted sampled-only image with retained guest bytes, no real writer and no
alias group needs no reverse encoding. It restores CPU protection without
inventing a readback serial. This is essential to avoid rejecting preserved
compressed-texture guest data.

The test's old guest byte is an explicit stale sentinel, not a captured game
texel. This proves the source-level race and its correction. **It does not prove
that the race occurred in one of the supplied SpongeBob boots**, nor that zero
pages explain the menu. The new APK tests this concrete candidate without
combining it with a memory-policy override or capture timing changes.

## RegisterAliases(memoryFreed) audit

Fourteen cases run in each Vulkan format-list mode, with actual native image
render/copy/encoding and host lifecycle facades:

| Writer/guest state | Implicit requested layout | Explicit requested layout |
| --- | --- | --- |
| Tight native writer, guest freed, producer submitted but not CPU-waited | Rejects before restore | Waits, restores bytes, registers |
| Tight writer with completed guest encoding | Registers | Registers |
| Converted/nonrecoverable writer | Rejects | Rejects |
| Freed data with no usable writer | Rejects | Rejects |
| Stale writer, guest generation behind group | Rejects | Rejects |
| Stale reference, newer member already published guest generation | Rejects early | Retires stale history and preserves current bytes |
| Padded freed writer | Rejects early | Returns success for logical texels but cannot reconstruct discarded padding |

The last case's FreeMemory facade clears its whole mirror; 24576 sentinel padding
bytes are lost while the real Vulkan logical pixels restore correctly. It is
not a measurement of Android's inward page rounding. The independently recorded
#395 memfd test supplies the real-discard precedent.

Therefore simply removing !memoryFreed or moving the implicit eligibility check
past restoration is **not a safe complete fix**. A general admission change
needs proof of which discarded bytes the writer can reconstruct, including
padding, and a valid authoritative generation. Those guards and registration
semantics are left intact in this candidate. The earlier failed fixture is
preserved as a negative result, not converted into a false passing assertion.

## Reference implementations inspected

- Kenji-NX source master `a6a641ada215837297e7170c91c6b4e8275574c6`, Android branch
  `280c427c550c5fe3bcc46b378e06a9b90a7e6b09`; Ryubing
  `69f2c0942452637b7be6965efea390f1868b8d9c`.
  `TextureGroupHandle.Sync` waits for the relevant registered sync when ready,
  preserves Modified for newer modifications, and keeps it set when a sync has
  not been created. SignalModified propagates dependencies;
  TextureGroup.SynchronizeMemory considers incompatible overlaps and deferred
  copies. Android/main group source differs; no APK provenance identity is claimed.
- Eden mirror `989768aa46720e3fcab9e2111e3ddd70819c382b`,
  `src/video_core/texture_cache/texture_cache.h`: PrepareImage separates refresh,
  alias synchronization and actual modification; SynchronizeAliases orders
  region-aware copies by modification_tick. DownloadMemory uses runtime.Finish
  before swizzling guest bytes. It clears an internal modified flag earlier;
  that alone is not the Strato trap race because the surrounding access and
  scheduling architecture differs.

These mechanisms support preserving current data through completion. No field,
cache policy or dependency graph is copied without its surrounding invariants.
The observed improvement in another emulator is not proof of one Strato fault.

## Validation, limitations and next device experiment

- Baseline gated publication test: fails before the copy, guest validity already
  published. Baseline eight concurrent traps: 8/8 prematurely permit access.
- Corrected readback suite: ASan/UBSan pass; 8/8 traps refuse while paused and retry
  after completion. Six injected failure stages with/without groups retain
  ownership and generations. Converted retained guest, inactive aliases and
  repeated-use controls pass.
- Alias audit matrix: 28 cases, real Vulkan synchronization validation, zero errors.
  This records existing rejections and padding loss; it does not claim those
  defects are repaired.
- Backing/alias/promotion regressions pass with ASan/UBSan and zero Vulkan errors
  **except the explicitly skipped preexisting CheckFreedWriterGuestBridge**.
  Its independent TEXMAN_FREED_ONLY run remains red. No unqualified complete-suite
  pass is asserted.
- Sampled layout, trap-manager and submission regressions pass. The test facade
  needed current sparse declarations and cancellation of unused, unsubmitted
  Clean-only command buffers; no production sparse behavior changes.
- No Android build or visual/performance validation was performed locally.
  Leak detection is disabled on the host; no leak-freedom claim.

Commands and test boundaries: `tests/gpu_guest_readback/README.md`. Evidence:
`docs/checkpoints/cosmic-freeguest-20261122/`.

The one necessary APK is the clean ARM64 Release publication candidate. Keep the
same Turnip/game/cache policy, verify menu stability over multiple full boots
with Free Guest OFF, then use ON as a separate control. Do not repeat the #478
or #479 forced policies. Check gameplay and warm FPS/frametime; one good boot is
not confirmation. A failed or partial result leaves the overall cause open.

If the setting gap persists, the smallest next trace should follow one bounded
**discard -> recovery -> first consumption** window: actual full-page range and
madvise result, guest-validity/serial/generation before and after recovery, and
the first upload/reuse that consumes that same physical span. Include whether
the producer was submitted and whether the consuming texels touch unrecovered
padding. Cap watched resources/events; do not force copies, submissions or
Mini-RenderDoc pass splitting. That can distinguish retained stale bytes,
unrecoverable discarded bytes and pending-writer reuse; GPU-MEMAB counts cannot.

Pinned source links: [earlier publication correction](https://github.com/BBlueSkyy/My-fork-Strato/tree/7c42375a86dd1d8e7694505d55a4bf4c4a0951cb),
[discard-safety audit](https://github.com/BBlueSkyy/My-fork-Strato/blob/ad1ed036/docs/guest-memory-recovery.md),
[Kenji-NX](https://git.ryujinx.app/projects/Kenji-NX/src/commit/a6a641ada215837297e7170c91c6b4e8275574c6/src/Ryujinx.Graphics.Gpu/Image/TextureGroupHandle.cs),
[Ryubing](https://github.com/Leuconoe/Ryubing/blob/69f2c0942452637b7be6965efea390f1868b8d9c/src/Ryujinx.Graphics.Gpu/Image/TextureGroupHandle.cs),
[Eden](https://github.com/eden-emulator/mirror/blob/989768aa46720e3fcab9e2111e3ddd70819c382b/src/video_core/texture_cache/texture_cache.h).
