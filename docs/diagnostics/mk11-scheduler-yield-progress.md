# MK11 / scheduler yield progress — 2026-10-18

Base: `master` `30f7244d3db8b20c168ea0f27c2375d6517af4b5`.
Diagnostic evidence: [PR #361](https://github.com/BBlueSkyy/My-fork-Strato/pull/361), head `827c0fc6dd38c21809b0af7f5a2d2d41e261dfce`, especially [the consolidated comment](https://github.com/BBlueSkyy/My-fork-Strato/pull/361#issuecomment-6254657050).

## Finding and proof boundary

**Confirmed: a general scheduler starvation scenario. Not yet confirmed: MK11 boots with this change, or starvation is its only remaining blocker.**

The supplied captures show HOS-1 at priority 35, C0, affinity C0/C1, yielding `-1` 4096 and 262144 times. It is waiting in Wwise's `GetSpeakerConfiguration` after waking the event consumer. `[0x8094D7878] == 0` is the first field of `CAkOutputMgr::m_listDevices`, not a separately proven initialization flag. HOS-32's argument identifies the Wwise event thread; its priority is 43 and affinity is C0-only. Repeated load-balancing logs do not prove that it has executed its guest entry point.

On the original scheduler, `SleepThread(-1)` wakes a parked candidate, rotates only within the caller's priority, and waits. The priority-35 caller remains first on C0. Only an unscheduled thread tries timed load balancing; a C0-only worker cannot move itself, and a caller that is already scheduled never enters that balancing loop. Therefore an allowed idle C1 can remain unused indefinitely while the C0 worker cannot run.

The Linux regression compiles the **production scheduler methods, SleepThread SVC, and adaptive condition variable**, rather than a copied scheduling model. The original master fails after 4096 migration yields with exactly this two-thread topology. The fixed scheduler moves the yielding caller to idle C1 and the C0 worker passes its real WaitSchedule predicate. This proves the scheduling defect, conditional on idle allowed capacity. The diagnostic captures do not contain a complete contemporaneous C1 run queue, so they cannot by themselves prove that exact topology existed throughout MK11's stall. No guest Wwise execution, event-consumption milestone, IPC response, first frame, or device performance result is claimed here.

## Implementation

- `kernel/svc.cpp::SleepThread`: `-1` requests cooperative caller migration after rotation. `0` remains local rotation; `-2` retains unbound parking semantics. `SetThreadCoreMask` requests reconsideration for an unbound thread without indexing the parked sentinel.
- `kernel/scheduler.{cpp,h}::MigrateToIdleCore`: moves only the calling host thread, under its migration mutex and both queue locks. Destination must be idle and in affinity; source must contain another runnable thread. It neither relaxes priorities nor moves another host thread that may still execute guest code.
- `ParkThread` / `WakeParkedThread`: validate allowed cores and priority handoffs, retain parked entries until actual enqueue, clear consumed entries, skip paused candidates, handle a sole thread, and propagate unused idle-core notifications. A contended migration try-lock records a condition-variable request; it does not wait for another running thread's migration mutex. Rechecks preserve busy-core priority handoffs as well as idle requests.
- `InsertThreadLocked`: shares the existing insertion/preemption logic with the atomic parked enqueue. An eligible equal-priority yield suggestion is inserted at the front, so delayed host wakeup cannot miss the caller's earlier rotation.
- `UpdateCore`, `UpdatePriority`, `PauseThread`, `ResumeThread`, `RemoveThread`, `MigrateToCore`: maintain parked state and trigger progress when affinity, pause, exit, or migration changes available capacity. `Rotate` handles an empty queue after a forced pause.
- `TimedWaitSchedule`: releases the core lock before acquiring migration, matching the existing untimed path's lock order.
- `kernel/types/KThread.h`: two small fields under the parked mutex hold deferred recheck requests; no guest ABI or saved-register layout changes.
- `common/spin_lock.h`: `fallbackLock.unlock()` replaces unlocking its backing mutex directly, preventing the unique_lock destructor from unlocking that mutex a second time.

No Title ID checks, guest-memory writes, forced audio-thread selection, new sleep/timeouts, polling in production, diagnostic trampoline changes, or audio-service changes are included. Existing preemption priorities, 10 ms timeslice, signal handlers, NCE exit processing, JIT halt processing and KProcess synchronization are retained.

## Reference comparison

References were read on 2026-10-18:

| Reference | Relevant behavior | Adaptation / limit |
| --- | --- | --- |
| [libnx svc.h](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/svc.h) | Separates `0`, `-1`, `-2` into local yield, migration yield, forced load balancing. | Preserve three distinct operations. |
| [Atmosphère Mesosphère scheduler](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_scheduler.cpp) (retrieved blob `e39eab8c93f12bca538c6db332758b0b00413be7`) | `YieldWithCoreMigration` rotates the priority queue and considers suggested threads. Global idle-core selection can move a core's top thread when its source has another runnable thread. `YieldToAnyThread` makes the caller unbound. | This is a Horizon reimplementation, not direct inspection or execution of Nintendo's kernel. The fix adapts the idle-core effect to Strato's host-thread model, moving the caller cooperatively. |
| [Eden scheduler](https://github.com/eden-emulator/mirror/blob/master/src/core/hle/kernel/k_scheduler.cpp) (retrieved blob `3156a170646f053eb53ed16121c697e53dfb89a1`) | Same priority rotation, suggested-thread migration, global idle-core fallback and unbound `-2` behavior. | No claim that `-1` automatically moves the caller every time, or lets lower priorities preempt it on the same core. |
| Yuzu lineage | Eden's inspected file preserves the Yuzu implementation and copyright; the comparison above covers that inherited algorithm. | A separate historical Yuzu source could not be fetched through the available endpoints; no independent modern Yuzu run is claimed. |
| [Ryujinx archived source](https://github.com/bachtran02/Ryujinx/blob/master/src/Ryujinx.HLE/HOS/Kernel/Threading/KScheduler.cs) | `YieldWithLoadBalancing` reschedules by priority and transfers eligible suggested candidates; `YieldToAnyThread` transfers the caller to active core -1. | Read archived implementation; no Ryujinx device comparison or copied scheduler code. |

This does **not** replace Strato's scheduler with Horizon's global suggested queues. General migration of other queued threads and arbitrary cross-core matching are outside this localized correction. A C0-only priority-35 thread may still legally starve a C0-only priority-43 thread with `0` or `-1` when there is no other permitted capacity; priority enforcement is intentional.

## Reproducible validation

```sh
python3 tests/kernel_scheduler/run.py
STRESS_RUNS=50 python3 tests/kernel_scheduler/run.py
ASAN_OPTIONS=detect_leaks=0 SANITIZE=address,undefined python3 tests/kernel_scheduler/run.py
# Negative controls against the original production code:
SCHEDULER_BASELINE_REF=origin/master python3 tests/kernel_scheduler/run.py migration
SCHEDULER_BASELINE_REF=origin/master python3 tests/kernel_scheduler/run.py park-affinity
SCHEDULER_BASELINE_REF=origin/master python3 tests/kernel_scheduler/run.py notifications
SCHEDULER_BASELINE_REF=origin/master python3 tests/kernel_scheduler/run.py park-alone
```

The first negative control reports C0 starvation; the second triggers UBSan on the original null `nextThread`; the third detects an unmatched unlock with test-only pthread ownership wrappers; the fourth reaches the test harness's process deadline. Test deadlines/rendezvous are harness-only. The production patch adds no scheduling timeout.

The 22 cases cover the priority-35/43 topology and 4096 repeated yields; all three SVC modes; pinned and flexible masks; occupied destinations; unproductive sole-thread migration; equal-priority rotation; eight concurrent workers; timer arming/disarming and yield flags; timed affinity migration; 1000 condition-variable exchanges; parked affinity changes, pause/resume, contended idle and busy-core handoffs, delayed enqueue, two simultaneous idle-core requests, and capacity freed by migration or pause.

Host facades replace guest execution, signal delivery, timers, logging and the spin instruction with Linux primitives. They test scheduler decisions, flags and timer requests, **not** Android signals, real AArch32/NCE execution, Wwise, or FPS/frametime. ASan runs disable LeakSanitizer because this environment cannot inspect its process task directory; the address and undefined-behavior checks remain enabled.

Additional unchanged CI gates were run locally: texture view ranges, GPU queries, conditional rendering, DRAW_TEXTURE, sampled layouts/aliases, and GPU submission. No Android APK was built locally. The PR workflow runs the scheduler host gate and requests only `:app:assembleDevReldebug` (ARM64 is already the app's ABI filter) for this branch or `debug/` branches, preserving Gradle/CCache optimizations. Other PR branches retain both APK variants and the existing GPU host gates. Actions is deliberately not monitored after publication; no merge.
