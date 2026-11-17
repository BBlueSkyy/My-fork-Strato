# DS Sync investigation and presentation ownership fixes

Baseline: `master` at `3065a7e3a85633d6b3faffd4bcefa6c171b5cbbb`.
Branch: `fix/ds-sync-presentation`. This change does not include or modify PR #349,
its shader-compiler gitlink, or the independent Texman PR #244.

## What can be established about the name

The supplied Discord quotation of TheASV, dated 2024-02-26, calls an unfinished
feature **DS sync** and associates it with the presentation engine and several
games. It does not expand the abbreviation. Searches of public Strato/Skyline
commit messages, source, public branch refs and indexed historical material did
not identify a definition, register, API, or published implementation bearing
that name. Consequently neither “Depth/Stencil” nor “Display Sync” is established,
and this PR does not claim to implement that unidentified feature.

The [official 2023 recap, published 2024-01-13](https://strato-emu.github.io/posts/recap-2023/)
describes TheASV's texture variants, on-demand texture synchronization and a
UsageTracker. It does not mention DS sync. Equating the two would be speculation.
The upstream public refs examined were master `ae1566a48285816a87e81d4aeb40bd2f4e56e60b`
and jit `fed1b8b773f0b87bf0e31e677226cd388baa8c03`; no public texman branch was exposed.
TheASVigilante/skyline's `present` branch ends at
`a74eb8dbd68cf7177c327222203fc7111a630f57` (2021); it is not evidence of a 2024 implementation.
Absence from these sources cannot rule out private or deleted development.

## Verified public history

Dates below are Git **author dates** (committer dates can be later).

| Date | Commit | Implemented behavior |
| --- | --- | --- |
| 2022-06-09 | [3139889a](https://github.com/strato-emu/strato/commit/3139889a09c294f9b72a93897340f8cfcbd260ed) | Asynchronous presentation |
| 2022-10-16 | [1a0819fb](https://github.com/strato-emu/strato/commit/1a0819fb76ec89e1ef2e9bf98af29c2c7873f9e6) | Presentation semaphore synchronization, reducing CPU waits |
| 2022-12-27 | [b3f7e990](https://github.com/strato-emu/strato/commit/b3f7e990cc100c939e84c68f2b3e08b8c3737395) | Tie guest GPU synchronization to host GPU completion |
| 2022-12-27 | [90e21b0c](https://github.com/strato-emu/strato/commit/90e21b0ca1a066a2d8e7c78facc65025612358aa) | Separate guest and host syncpoint counters |
| 2022-12-27 | [cc3c869b](https://github.com/strato-emu/strato/commit/cc3c869b9f16198b192799e110742ec68c8d03e9) | Signal guest vsync near presentation |

Reproduce the history search with `git log --all --oneline --regexp-ignore-case
--grep='DS sync'`, `git log --follow -- app/src/main/cpp/skyline/gpu/presentation_engine.cpp`,
`git show <commit>` and `git blame` on the functions described below.
Existing 2022 synchronization is not proof that the unfinished 2024 feature exists today.

## Current dependency chain

* GPFIFO/Maxwell syncpoint increments advance the guest counter while recording;
  `CommandExecutor::AddDeferredAction` defers host increments to GPU completion.
  `AndroidFence::Wait` observes the host counter, not the recording counter.
* `GraphicBufferProducer::QueueBuffer` marks a slot queued and hands a texture,
  native fence and release callback to the asynchronous presentation queue.
* `PresentationEngine::PresentFrame` waits for the supplied host1x fence,
  reconciles texture aliases and synchronizes host backing. Swapchain acquisition
  supplies a binary semaphore; `Texture::CopyFrom` records the copy with the
  existing source-cycle dependency and signals the presentation semaphore.
* The scheduler serializes queue access with its existing queue mutex and submits
  cycle fences/semaphores. `FenceCycle` tracks submission, completion, chained
  dependencies and resource retention. Per-frame acquisition semaphore reuse is
  guarded by the corresponding copy fence. `presentKHR` waits on the copy signal.
* The presentation thread invokes the producer callback after `PresentFrame`.
  Swapchain presentation and resource completion are distinct events. These fixes
  preserve the existing GPU dependency chain; they concern producer slot ownership.
* VIC operations execute synchronously before THI OpDone signaling. The audit did
  not establish a missing VIC wait or an incorrect guest/host increment order.

No unconditional queue/device idle, additional submission, copy, GPU barrier,
thread, sleep, timeout, global lock or shader compilation step is introduced.

## Proven defects and fixes

1. **Stale release after slot reset.** A queued callback captured a reference to
   the slot and unconditionally made it free. Disconnect or SetPreallocatedBuffer
   could reset that slot and queue another frame before the old callback ran.
   The old callback would then free the newer queued/dequeued buffer. Release now
   requires both the captured global frame number and the queued state to match.
   The producer's global frame counter is not reset by these slot operations.
2. **Unlocked texture handoff.** QueueBuffer read `buffer.texture` after releasing
   the producer mutex. Slot reset could change it during that handoff. A local
   shared ownership snapshot is now taken under the existing lock and handed to
   Present, preserving the chosen TextureView during concurrent reset.
3. **Cancellation omitted dequeuer notification.** CancelBuffer made a slot free
   and signaled the guest event but omitted `freeCondition.notify_all()`, although
   DequeueBuffer waits on that condition. It now notifies after making the slot
   available. The existing native fence wait and guest fence values are preserved.

These are deterministic presentation defects. No evidence demonstrates that NieR
triggers them or that they caused the supplied Vulkan device loss.

## Reference implementations

Eden `14da4482a4668928bad43bb67ef12f96b9c9d972` and Yuzu
`7ffac53c9e67fa18d067114a8eb3e77c74bba68f`,
`src/core/hle/service/nvnflinger/buffer_queue_consumer.cpp::ReleaseBuffer`, reject
a mismatched release frame number and validate consumer ownership before freeing
the slot. Their producer cancellation signals the dequeue condition.
Ryujinx mirror `kotx/Ryujinx` at `534f92506bf8c6aba29bb86917bf10954a360d8e`,
`src/Ryujinx.HLE/HOS/Services/SurfaceFlinger/BufferQueueConsumer.cs::ReleaseBuffer`,
likewise validates frame number and acquired state. These support the ownership
invariant; none establishes an equivalent feature named DS sync.
Strato combines producer/consumer handling and uses Queued until its presentation
callback, so importing another emulator's Acquired-state check verbatim is wrong.

## NieR validation log: separate unresolved rendering problems

Examined supplied `emulation(20261016-220007).log`: Adreno 650,
Qualcomm proprietary driver 0.805.0, Vulkan 1.1.295. Counts are **log lines**,
not duplicated occurrences of the VUID text within each line.

| Evidence | First timestamp (microseconds) | Lines | Interpretation |
| --- | ---: | ---: | --- |
| RuntimeSpirv-None-06343 | 416025 | 8470 | Geometry-stage subgroup operations without that stage in supportedStages |
| vkCmdDrawIndexed-None-04584 | 56165865 | 674 | Sampled descriptor binding 4/index 0 overlaps framebuffer attachment 0 |
| VkImageViewCreateInfo-viewType-01004 | 79899617 | 2 | Cube-array view created without enabled imageCubeArray |
| ErrorDeviceLost | 82403920 | — | First reported by presentation queue submission |

`TraitManager` currently derives subgroup vote support from supportedOperations,
without stage filtering; `ShaderManager` supplies one shared profile. This is a
real capability-model limitation, not evidence of a presentation sync failure.
`imageCubeArray` is absent from enabled features while cube-array views are
supported by the texture/view code. A feature-aware generic fallback is a separate
rendering change. The overlap violation requires resolving same-draw attachment
feedback semantics; a queue wait or barrier cannot legalize that overlap.

The post-loss device-fault query reported Incomplete, empty description, zero
address/vendor entries. It does not identify a faulty instruction or resource.
The first thread observing device loss does not identify the command that caused
it. Neither a Qualcomm driver defect nor any specific one of these API violations
is established as the sole cause. No shader replacement comparison was run here.
Those rendering changes belong in a separately justified scope rather than being
represented as an implementation of the unidentified DS mechanism.

Other audit leads remain outside this patch: the syncpoint manager's `<=` boundary
check, channel syncpoint retirement before outstanding work has completed, and
Syncpoint::Wait's already-reached boolean result. The latter is ignored by current
presentation callers. Channel retirement still requires complete lifetime tracing;
no speculative global wait or changed allocation policy was added.

## Validation and limitations

Host tests compile unchanged production producer method bodies against lightweight
platform interfaces, including controlled slot reset during the unlock-to-Present
handoff. They model ordering/lifetime, not a Vulkan presentation surface.
Host1x tests compile the real syncpoint class/source and cover threshold order,
callback deregistration, immediate completion and guest/host counter separation.
Existing occlusion, conditional rendering and DRAW_TEXTURE regression suites are
also run. Android PR Build runs these regressions before the ARM64 assemble step.

Validation results: all 8 producer cases pass with UBSan; the master revision
fails the 7 intended regressions and passes ordinary fence continuity. Host1x
passes with ASan/UBSan. Existing occlusion, conditional rendering and DRAW_TEXTURE
host regressions pass. `git diff --check` passes. The host1x compiler reports the
pre-existing constexpr atomic-load warning in `Syncpoint::Load`.

The host benchmark is a synthetic QueueBuffer/release CPU test, not FPS or GPU
frametime. With GCC 13.3, `-O2` and UBSan, 10,000 warmup iterations and five
1,000,000-iteration samples per revision, results in ns/iteration were:

| Revision | Samples | Median |
| --- | --- | ---: |
| master 3065a7e3 | 188.518, 194.172, 313.219, 317.127, 194.818 | 194.818 |
| fixed | 186.490, 167.397, 174.881, 172.636, 171.877 | 172.636 |

No regression was observed in this harness. The baseline variance is large; these
measurements do not establish a speedup or a bound on Android overhead. Reproduce
using `python3 tests/hosbinder/run.py --ref 3065a7e3 --benchmark` and
`python3 tests/hosbinder/run.py --benchmark` on an otherwise idle host.
 Shader compiler, pipeline cache, GPU submission count and GPU copy count
are unchanged by the production diff. Real FPS, frametime percentiles, CPU/frame,
presentation time, fence waits and memory measurements require Android runs.

No Moto G100/Turnip/proprietary-driver game run is available in this environment.
NieR is **not confirmed fixed**, and absence of device regressions cannot be
guaranteed by host tests. Test master and this branch on identical firmware,
driver/settings, replacement files and save/scenes, with warm/cold shader caches
recorded separately; collect multiple equal-length runs after warmup and compare
median FPS, mean/p95/p99 frame time, CPU/frame, submissions, fence waits and memory.
Retest known-working games, reconnect/preallocate paths and surface recreation.
Test Geometry PR #349 separately or in an explicitly identified local combined
build; this master-based branch does not silently include that unmerged PR.

The build is requested by opening the Draft PR through the existing PR Build
workflow. Completion and APK installation are left to the user; no polling or
artifact download is performed.
