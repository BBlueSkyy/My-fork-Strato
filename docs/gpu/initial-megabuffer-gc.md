# Initial GPU garbage collection: aged megabuffer surplus

Base: master `d54d40e1cc9656a4bb377931aa70e490c95ffc25` (10 November 2026).
Scope: completed surplus in `MegaBufferAllocator`, building on existing fence collection.

## Architecture audit

| Resources | Existing ownership and retirement | Decision |
| --- | --- | --- |
| Submission dependencies | `FenceCycle` retains shared objects/chained cycles; `CommandScheduler`'s existing waiter calls `Wait(true)` through a 256-cycle bounded queue. Cancel releases dependencies for work that will not be submitted. | Reuse; no second queue, worker or completion system. |
| Guest buffers | `BufferManager` owns mappings/page table; buffers coalesce and retain predecessors via cycles. Views/delegates and dirty guest/host state participate in coherence. | No age/use-count eviction: inactivity alone does not prove guest data can be discarded. |
| Guest textures and views | Manager mapping index owns textures; aliases can hold authoritative GPU data. Backing promotion retires old framebuffer/view/image generations with existing fence ownership. Texture destruction may synchronize guest memory. | No generic eviction or changes to Free Guest Texture Memory. Safe general eviction needs a separate authority/trap/dependency design. |
| Upload staging | Shared staging buffers are attached to synchronization/submission cycles. Download staging is retained per texture for reuse. | Preserve existing retention/reuse. |
| Descriptor sets/pools | Active sets own pools until fence dependencies release; idle sets are reused. Master already resets a sole-owner idle pool before growing (`84c048c5`). Replaced pools survive through active owners. | Existing partial collection is sufficient for this step. |
| Render-pass/framebuffer caches | Stable handles are used by recorded work; fixed framebuffer retirement is already targeted by old view generation. | No blanket size/age invalidation without per-entry ownership proof. |
| Megabuffers | 25 MiB chunks are first-fit reused after the cached fence-completion flag; the original list never removes a chunk before allocator teardown. A tail unused after a temporary burst stays allocated indefinitely. | Concrete initial reclamation target. |
| Presentation/swapchain | Existing image wrappers, presentation fences and semaphore ownership remain independent. | No swapchain policy changes. |

Recent master work on sampled-layout aliases, backing promotion, idle descriptor reset, subgroup stages and Warp32 was inspected. PRs #400, #420 and #426 were read for scope: no commits were imported and none of their branches were modified. PR #426 now separates BOTW probes into #440. This implementation does not rely on that instrumentation.

The pool/fence design is inherited from Skyline; source history already supplies the compatible reuse boundary. Importing another emulator's general texture collector would require a different guest-authority/lifetime design and is unnecessary for this scoped change. No graphical bug is claimed fixed.

## Collection policy

- Keep at least four already allocated chunks (100 MiB). This is a floor for trimming, not preallocation: a one-chunk workload stays at one chunk.
- Require 256 **pool rollovers** without use. This is neither frames nor elapsed time. Stamp chunks only when entering/leaving the active slot.
- Inspect one candidate per rollover using a stable list cursor. The pre-existing first-fit reuse scan is unchanged; the collector adds no full scan.
- Exclude the active chunk, recent chunks and chunks whose existing `TryReset()` cannot prove completion. That check uses `Poll(true)`: no new driver fence-status query or GPU/CPU wait.
- Splice the completed node into a shared list attached to the current recording cycle. The existing cycle waiter frees its Vulkan/VMA buffer on completion. Cancellation may release it immediately: its original GPU users had already completed before retirement.
- Queue at most one retirement. A weak ownership check throttles collection if the receiving submission stalls. `weak_ptr::expired()` can become true while the prior driver's destructor is still executing, so this is a bound on queued ownership, not a strict one-allocation physical-memory bound until destructor return.
- Allocate the retirement wrapper and fence dependency **before** removing pool ownership. If either throws `std::bad_alloc`, skip collection and keep the usable chunk. No collection work is added to successful ordinary allocations.

The current cycle must still be recording, as required by existing allocator callers. Already completed/cancelled cycles skip collection. The local strong owner protects the detached list until splicing finishes; candidate `TryReset()` drops the old completed cycle before the new dependency is installed, avoiding a retirement/cycle ownership loop.

Per-execution cached raw bindings are refreshed: Maxwell3D's flush callback attaches old descriptor storage, clears descriptor state and marks active state dirty; `Buffer::TryMegaBufferView` resets its table on execution-tag changes. Zero-sized transform-feedback allocations still pass through `MegaBufferChunk::Allocate`, which records/chains the current cycle. This is inspected callsite evidence, not an integrated renderer test.

No new thread, wait-idle, resource-wide cache invalidation or logging. New heap allocations happen only on actual aged retirement, not on ordinary allocation or every rollover. Live GPU working-set demand can still grow the pool. Idle sessions without further rollovers intentionally do not collect. Collection is bounded by resource count; Vulkan driver destruction has no proven wall-clock bound. The four-chunk/256-rollover defaults have not been tuned on Android.

## Local validation

`tests/gpu_megabuffer_gc/run.py` compiles the **complete production header/source** and real `FenceCycle`/`AtomicForwardList` with host Vulkan-fence and GPU-memory facades, C++20 and `-Wall -Wextra -Werror`. It does not implement an independent allocation/collection algorithm. Private access is exposed only in a temporary test header for targeted faults/invariants.

| Check | Result |
| --- | --- |
| New GC suite, ASan + UBSan | 28 checks pass: grace/floor, pending/submitted/chained fences, multiple submissions using one chunk, deferred destruction on a completion worker, cancellation, allocation failure at wrapper/dependency sites, active/recent exclusion, cursor reuse, no steady-state reallocations, byte copying/alignment and balanced destruction. |
| Same test against unmodified `d54d40e1` | Expected reclamation failures; burst capacity stays at 8 chunks / 209,715,200 bytes. Changed implementation reaches 4 chunks / 104,857,600 bytes. These are harness backing capacities, not measured Android GPU memory or process RSS. |
| `tests/gpu_submission/run.py` | Pass, 26 checks. |
| `tests/gpu_queries/run.py` | Pass, 15 checks. |
| `tests/gpu_draw_texture/run.py` | Pass, 16 checks. |
| `tests/gpu_sampled_layout/run.py` | Pass, sampled-layout and alias checks; existing shared_ptr atomic deprecation warnings remain. |
| `tests/gpu_null_storage_image/run.py` | Pass. |
| `tests/gpu_texman_multirange/run.py` | Pass, three binaries. |
| `tests/gpu_conditional/run.py` | Existing fixture compilation failure: facade `ActiveState` lacks `InvalidateIndexBuffer`. Reproduced identically from a clean archive of master `d54d40e1`; affected files are unchanged. |
| `tests/gpu/run_pitch_linear_tests.sh` | First arithmetic test passes; existing integration fixture lacks `GetBackingLayerCount`/`backingLayerCount`. Reproduced from a clean master archive; affected files are unchanged. |
| `./gradlew --offline --no-daemon assembleDevRelease` | Blocked before project compilation: wrapper attempts to download uncached Gradle 8.2, then `java.net.SocketException: Network is unreachable`. SDK/NDK is also absent. No local APK build is claimed. |
| Diff whitespace and Python syntax | Pass. |

LeakSanitizer cannot inspect this ptrace/container environment. Existing null/texman attempts initially reached that limitation and pass with `ASAN_OPTIONS=detect_leaks=0`; ASan/UBSan remain enabled. The GC fixture separately checks all backing allocations are destroyed exactly once. This is not a global renderer leak proof or ThreadSanitizer race proof.

An independent read-only review found no Critical/Important blocker; its ownership-bound precision finding is documented above. Worker tests join completion promptly, so concurrent recorder/driver-destructor overlap is inspected rather than covered by a renderer stress test. Existing shutdown drains record/completion workers before cancellation; arbitrary cancellation of a live consumer is not asserted safe by these tests. Device loss and real Android teardown retain their existing behavior and remain unvalidated here.

## Host CPU comparison

Commands (five repetitions each, GCC 14 `-O3`, same test runner):

```
python3 tests/gpu_megabuffer_gc/run.py --benchmark --repeat 5 --baseline d54d40e1
python3 tests/gpu_megabuffer_gc/run.py --benchmark --repeat 5
```

| Host measurement | Master | Candidate |
| --- | --- | --- |
| Ordinary allocation, ns/op, observed range | 6.84–7.50 | 6.83–7.24 |
| Rollover p50, ns | 30 | 30 |
| Rollover p95, ns | 30 | 31 |
| Rollover p99, ns (range across runs) | 31–40 | 40 |
| Maximum observed rollover, ns (range across runs) | 80–11,577 | 751–1,002 |

The ordinary allocation body is unchanged. These noisy host microbenchmarks show a small additional slow-path cost; they are not evidence of improved emulator performance or absence of Android CPU/FPS/frametime regression. Driver allocation/free cost is absent from the facade measurements. No per-frame timings or percentiles have been measured.

## Device validation and publication

The existing **PR Build** workflow already handles `pull_request: opened` against master and builds `assembleDevRelease`, verifies only AArch64 native libraries and uploads `strato-pr<PR>-arm64-release.apk`. The focused GC regression is added to that workflow; no new Android workflow or build variant.

Opening the Draft PR is the final action and requests this workflow automatically. Do not monitor its status, await artifacts, fix remote failures, or merge. A build/run URL is not available from the PR-creation response; the workflow/branch page can be linked without querying run status.

On Moto G100 compare the same master/candidate scene, settings and driver after warm-up, then run prolonged/burst workloads and record GPU/process memory, allocations, CPU time and mean/p95/p99 frametime. Cover BOTW, MK8, Link's Awakening, Echoes of Wisdom, Pokémon Z-A and FE3H with Qualcomm and Turnip where each boots. Also exercise game exit/restart and swapchain recreation. FPS, stutter, game compatibility, actual VMA release cost and all device-loss behavior are pending. This is initial GC for megabuffer surplus, not general texture/buffer cache eviction.

## Per-game GC diagnostics

The normal game shutdown path now emits exactly one INFO `[GPU-GC]` summary to
`emulation.log` immediately before logger finalization. The line includes game name,
title ID, megabuffer pool rollovers, chunks allocated during the session, peak and
currently pooled chunks, candidates inspected, candidates rejected by an incomplete
fence, retirement requests queued, retirement allocation failures, and peak/current
pool backing capacity in MiB. This identifies both games that trigger collection
and games that only use/reuse megabuffers.

Counters are atomic for a race-free snapshot and updated only during the existing
rollover, growth and retirement paths. Successful ordinary allocations perform no
new diagnostic work. A retirement count is **scheduled deferred destruction**, not
confirmed Vulkan-driver memory release; peak/current capacities refer only to
25 MiB megabuffer backings, not overall process RSS/VMA budget. A game can show
zero retirements because it never grew above the reserve, never aged candidates for
256 rollovers, or still has GPU work pending. The one-line snapshot is not a proof
of memory reclamation or FPS behavior. Abrupt native process termination may bypass
normal shutdown and therefore omit the summary.

Example (illustrative only):

```text
[GPU-GC] game="Example Unity 2D" title_id=0100000000000000 rollovers=520 allocated_chunks=8 peak_chunks=8 pool_chunks=5 candidates_inspected=250 pending_fence=0 retirements_queued=3 retirement_alloc_failures=0 pool_capacity_mib=125 peak_capacity_mib=200 (retirements are scheduled, not confirmed driver frees)
```

The GC grace period, reserve floor, fence handling and collection cursor algorithm
remain unchanged. Android FPS/frametime and Qualcomm/Turnip validation remain pending.

## In-game progress snapshots

Some Android games (including BOTW) can exit without reaching the normal shutdown
logger path. While the title is running, the allocator now emits an INFO
`[GPU-GC] progress` line on its first megabuffer rollover and every 256th
rollover thereafter. This is event-driven, not a timer: games that never roll
over the megabuffer will not produce progress lines. The existing startup log
identifies the game, and the shutdown summary (when reached) includes its name
and title ID. No extra fence polls, allocation-path logging, or per-frame scans
are added. Check that the APK contains this commit before comparing logs.
