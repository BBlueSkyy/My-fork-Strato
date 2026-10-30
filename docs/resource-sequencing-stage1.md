# Resource sequencing: bounded stage 1

Base: `master` `9935f13349c00d732ae599f4f90866b5b79629f5` (2026-10-25).

This branch fixes four demonstrated ordering/lifetime failures in existing paths. It is **not** the Strato developer's private sequencer, a complete memory-region authority implementation, or a demonstrated fix for UE4 graphics. The description supplied by the developer establishes the desired semantics, not an implementation available for reuse.

## Audit and boundary

| Subsystem | Existing behavior inspected | Finding / stage-1 action |
| --- | --- | --- |
| `CommandExecutor::SubmitInternal` | Resource locks persist through preparation; predecessor `WaitSubmit` precedes texture synchronization; buffers update and chain their cycles; worker threads record/submit nodes | Preserve this path. `WaitSubmit` orders queue submission, not GPU completion. No global scheduling replacement. |
| `UsageTracker` / GPFIFO | `dirtyIntervals` marks GPU writes for CPU command consumption; `sequencedIntervals` invalidates shader-cache reads after sequenced writes | Fragmented command snapshots occurred **before** the existing dirty flush. Move snapshot creation after it. Preserve the contiguous no-copy path, deferred macro flag and all existing flush/wait semantics. |
| `Buffer` / `BufferManager` | Staged read/write traps, direct-import shadow intervals, sequence/write generations, buffer coalescing and predecessor cycle handoff | Existing buffer-local ordering remains. This is not a unified buffer/texture authority ledger. DMA buffer writers already use these mechanisms; no speculative DMA change. |
| `Texture::SynchronizeHost[Inline]` | Dirty state is tentatively changed before CPU bytes are staged; standalone exact aliases already have pre-submit lifetime retention and rollback | Ordinary/inline upload exceptions left `Clean`/`GpuDirty`, so retry could skip the only valid CPU bytes and even discard them. Restore `CpuDirty`/previous layout on pre-submission failure. Retain staging and texture before recording/submitting for every texture. |
| `Texture::CopyFrom` / presentation | Waits for producer submission, uses the producer semaphore where available, waits for destination reuse, records copy/blit, attaches destination completion cycle | Source readers were absent from source cycle tracking when no usable producer cycle represented them. Publish the submitted copy cycle on **both** textures. Attach both lifetimes before submission and roll back destination layout on failed recording/submission. |
| `TextureManager` | Full/mip/layer/3D representable views reuse existing storage; exact layout aliases have generation/authority and submit/retry reconciliation | Preserve matching, caching and exact-alias policy. No new partial-overlap component or per-draw cache scan. |
| `TrapManager` | Try-lock callbacks under trap mutex; blocking resource preparation outside it; texture/buffer fence waits | Preserve lock ordering and callback contracts. No new trap-mutex wait. Updated source read cycle is visible to existing CPU backing-reuse/fence waits. |
| Vulkan scheduler / `FenceCycle` | Independent recording threads share serialized queue submission, completion chains retain lifetimes, semaphore reuse can introduce a reverse completion dependency | Do **not** chain the producer cycle into the copy cycle: producer semaphore recycling can already make producer completion depend on that copy. Preserve asynchronous submission. Correct the cancelled-slot semaphore state described below. |

## New mechanisms

### Reuse of cancelled command slots

A fresh slot cancelled before submission never signalled its binary semaphore. The old `nextSemaphoreSubmitWait` default nevertheless made its replacement wait on that nonexistent signal, so recovery could hang. For an unsubmitted cancellation, inherit `semaphoreSubmitWait` as the next slot's pending-signal state. Preserve a real unconsumed signal, and do not recreate one already consumed externally. Submitted cancellation behavior is unchanged. This prerequisite was found during independent review of the new pre-submit failure paths.

### CPU upload authority and failure recovery

Both upload methods retain the original guest bytes after staging/lifetime/recording failures and force a subsequent attempt to stage them again. Ordinary standalone uploads now use the existing alias path's pre-submit lifetime discipline. A failed standalone command buffer is cancelled; layout/cycle publication follows successful submission. This avoids an allocation failure after submission releasing resources still in use.

The successful upload policy is preserved, including CPU writes detected while staging, mapped uploads, alias initialization and Free Guest Texture Memory ON/OFF. No new lock, wait or GPU command is added. Inline uploads remain recorded into the executor's current command buffer. **If an inline record operation partially emitted commands before throwing, its caller must abandon that command buffer; this change does not implement in-place executor command-buffer rewind or automatic process recovery.** Restoring texture metadata alone is not such a rewind.

### Track GPU copy reads as resource uses

After a successful texture copy, the source's `cycle` identifies its latest GPU reader, as well as the destination's writer. Existing `WaitOnFence`, mapped backing writes, promotion and readback can then observe that reader. Producer readiness uses the existing submission-only wait; the change adds no CPU wait for GPU completion.

Image owners are retained before queue submission. Failures before successful queue submission do not publish the new resource cycles or destination layout. This changes the existing presentation copy path, so presentation smoothness and frame pacing require device validation.

### CPU command fetch follows its producers

A fragmented GPFIFO entry previously called `gmmu.Read` to build `pushBufferData`, then synchronized pending writes to those ranges. Waiting after taking that snapshot cannot update its bytes; with staged pending writes, entering guest access before submission may also encounter resource traps. The test establishes the stale snapshot case using a producer that updates its guest words during the existing flush. The fix moves that exact block, adding neither copies nor waits.

## References and intentionally pending work

- [#392](https://github.com/BBlueSkyy/My-fork-Strato/pull/392) and [#399](https://github.com/BBlueSkyy/My-fork-Strato/pull/399): readback publication and Linear `CopyToGuest` fixes. Neither implementation is repeated here; `SynchronizeGuest`, `CopyToGuest` and their traps are unchanged. They are not part of base master, so their known limitations still exist on this branch.
- [#393](https://github.com/BBlueSkyy/My-fork-Strato/pull/393): partial-overlap authority, masked publication and cached descriptor reconciliation are a different mechanism, with reported device regressions. No code imported.
- [#395](https://github.com/BBlueSkyy/My-fork-Strato/pull/395): reconstructible page discard and host-vs-guest address contracts. No code imported. Padding/shared-page eviction safety remains a prerequisite for a complete Free Guest Texture Memory guarantee.
- [#396](https://github.com/BBlueSkyy/My-fork-Strato/pull/396): partial storage-writer preflight. No code imported. Its documented boundary still applies: RT and storage writers, all descriptors and irreversible draw preparation must be coordinated before adding dependency retries.

Pending: a persistent physical-region authority ledger across buffer/texture/CPU accesses; compatible **partial** texture-overlap producer/consumer dependencies and A→B→A; transactional draw/dispatch writer publication; cached-binding invalidation at those boundaries; unmap/remap lifetimes; complete page-discard eligibility. Incompatible overlap layouts are explicitly outside this stage. Existing compatible views sharing a single `Texture` continue to share its cycles; separate partially overlapping `Texture` objects are **not** made coherent by these changes.

In particular, this branch prevents discard after the newly tested failed-upload path; it **does not guarantee every Free Guest Texture Memory path is safe**, and must not be described as complete CPU resource sequencing. OFF must also be tested; deletion of old guest bytes is not a correctness mechanism.

## Essential validation

- `python3 tests/gpu_texture_sequence/run.py`: unchanged production upload/copy/fence-wait bodies against host GPU interfaces. Red on master for lost CPU upload authority and untracked source-copy read; green with this branch. Injects staging, allocation, lifetime attachment, upload recording, end-recording and queue-submission failures; verifies retry/layout state, CPU-write detection during staging, eviction ON/OFF, pre-submit lifetimes, source CPU reuse blocking until copy completion, and submission-only producer readiness. GPU I/O/format/trap behavior is modeled; it is not Vulkan validation or a full Maxwell draw.
- `python3 tests/gpu_pushbuffer_sequence/run.py`: exact production mapping/flush/materialization block. Red on master for stale fragmented command words; green here. Covers clean/dirty contiguous/fragmented ranges, deferred macro flags, no unnecessary clean flush, failed producer flush and retry. Executor/GMMU/interval interfaces are facades; no ARM64 signal handler is executed.
- `python3 tests/gpu_submission/run.py`: existing production FenceCycle/SubmitInternal/buffer-cycle regression under UBSan. Adds real production-header cancellation/reuse checks (fresh, pending-signal, consumed-signal and repeated cancellation) and a producer/copy/subsequent-upload completion graph. The slot state check inspects the private flag actually used by the scheduler, via its friend; it does not submit real Vulkan work. Also adds the fixture layout member required by upload rollback.
- `git diff --check`; workflow/build configuration diff must be empty.

Independent whole-diff review found the cancelled-slot recovery blocker; its regression failed before the fix and passed after it. The upload/copy and pushbuffer regressions also failed on master before implementation. All three final test commands passed. An additional ASan/UBSan run passed with `ASAN_OPTIONS=detect_leaks=0`; LeakSanitizer itself could not inspect `/proc/.../task` in this environment, so no leak-check claim is made.

No local Android APK build, device FPS result, Vulkan driver validation or mini-RenderDoc capture is claimed. Test ON/OFF under identical Release settings in SpongeBob: The Cosmic Shake, Dragon Ball: Sparking! ZERO, Bulletstorm and Dragon Ball FighterZ, including repeated presentation, menus and gameplay. Keep Draft and do not merge without device results. Existing `assembleDevRelease`, `arm64-v8a` and APK artifact workflow are unchanged; no RelDebug, new mandatory CI tests, or Actions monitoring.

## Follow-up: unnecessary layout wait after presentation (FighterZ performance investigation)

`TransitionLayout(General)` waited on and cleared the resource cycle before checking whether the image was already General. Storage descriptors request this layout repeatedly. Since this PR tracks presentation reads on the source texture, that no-op could wait for presentation GPU completion during descriptor preparation.

Return after `WaitOnBacking()` when the layout is unchanged. Keep the existing cycle, copy-reader lifetime guarantees, real-transition wait and all GPU barriers. No reader/producer split, rollback of earlier mechanisms or driver-specific workaround is introduced.

The exact production-method test `python3 tests/gpu_texture_sequence/run.py layouts` was RED on #400 and GREEN after the early return: General→General returns while a pending reader remains incomplete, without a wait, cycle reset or submission; a different layout still waits and submits. The full texture-sequence and submission regressions pass. This removes a demonstrated unnecessary stall, **not a measured FighterZ FPS regression**. Compare the updated Release APK against the original #400 with identical driver, settings and scene before asserting FPS recovery.

The same Draft #400 now also contains the selectively reused #396 storage preflight and the producer-safe overlap materialization boundary. See [overlap-materialization-order.md](overlap-materialization-order.md) for tests, the independent-review correction, the explicitly failing live A→B→A oracle, and performance/region-authority limits. No separate PR was created for this continuation.
