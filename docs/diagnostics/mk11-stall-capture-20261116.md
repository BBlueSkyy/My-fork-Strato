# MK11: bounded capture of the unresolved scheduler stall

Baseline: PR #362 commit `9607cd788a2293aa7dbbfd3bc0121e6806a3458e`.
This follow-up adds evidence capture, not a scheduling fix. The preceding
[audit and swapchain correction](mk11-pr362-20261116-audit.md) remain intact.

## Evidence from the supplied Perfetto trace

Source: `trace-nio-S1RTS32.41-20-16-1-11-2026-11-16-08-40-49.zip`.
Trace Processor v59.1 reports bounds `134142393889880..134174592720285` ns,
duration 32.198830 seconds. Clock snapshots place the recording around
08:40:17–08:40:49 in America/Sao_Paulo. The emulation process is PID 10224.

In the interval `134153110000000..134174589249816` ns (21.479250 seconds):

| Host thread | TID | Running CPU time | Observation |
| --- | --- | --- | --- |
| Main HOS-1 | 10260 | 21.473227 s | Almost one fully occupied host CPU. |
| HOS-8 | 10308 | 5.640854 s | Continues waking and running. |
| Guest HOS-32 | 10335 | 0.004960 s | Mostly sleeps; periodically resumes very briefly. |
| GPFIFO | 10342 | 0 s | Sleeps throughout this interval. |
| Sky-CmdRecord | 10340 | 0 s | Sleeps throughout this interval. |
| Sky-CycleWaiter | 10266 | 0 s | Sleeps throughout this interval. |

HOS-32's preceding sleeps approximate 20, 40, 80, 160, 320, 640, 1280,
2560, 5120 and 10240 ms. This is a strong match for `WaitSchedule`'s existing
exponential load-balancing wait, but not a guest call-stack proof. No earlier
HOS-32 sleep in this trace exceeds 600 ms.

A driver `ir3_compile_shader_nir` slice lasts 8.396517 seconds and completes.
The other recorded shader compilation and Vulkan fence-wait slices complete;
there is no open application slice identifying a continuing shader/fence wait
through the final stall. The recording contains zero CPU call-stack samples,
no Android log records, and no exit event for this emulation process. Two
negative-timestamp records were dropped and one power-rail packet is empty;
these limitations are not hidden as a clean trace-health result.

This does not prove that HOS-1 stopped yielding. Repeated same-thread yields can
also consume almost a CPU if no permitted migration becomes productive. Linux
scheduling priorities and CPU IDs do not reveal Horizon guest priorities,
affinities or virtual-core queues. The earlier logs' differing driver versions
and pipeline counts do not form a controlled driver/cache comparison.

## Why capture before changing behavior

The current caller-only migration handles `SleepThread(-1)` when that caller has
another runnable source resident and a permitted idle target. A pinned waiter
in `WaitSchedule` does not migrate a foreign executing queue front. That
architectural boundary is compatible with the trace, but its required topology
is not yet observed. Priority inversion, an ordinary guest/host loop and
unproductive yields are still distinct possibilities.

Chosen approach: capture the waiting thread's queues and the observed fronts'
register context in the same diagnostic APK. Queue-only capture would require
another build if the PC were still missing; per-SVC logging, stack walking,
guest-memory probes or periodic sampling add unnecessary disturbance. No new
behavioral scheduler change is justified by the current evidence.

## Implementation and limits

- CMake/Gradle default **OFF**: `STRATO_SCHEDULER_STALL_DIAGNOSTICS` /
  `schedulerStallDiagnostics`. Normal builds compile out the hooks, mailbox
  fields, handlers and signal installation.
- The #362 workflow explicitly builds **ON** only for
  `fix/scheduler-yield-progress`, with generic waiter filter
  `schedulerDiagnosticThread=32`. Default filter 0 means any waiter. Filtering
  avoids spending the three-round budget on unrelated background startup
  threads. It changes diagnostics only, not game execution or priorities.
- At existing `WaitSchedule` timeouts of at least 640 ms, try to capture at most
  **three** rounds per scheduler. No new timer, timeout, polling or sleep is
  introduced. With uninterrupted exponential backoff, those rounds fall around
  1.26, 2.54 and 5.10 seconds after waiting begins. The following existing timeout
  can drain the last asynchronous response.
- Each round copies up to eight entries per virtual-core queue under a try-lock,
  retaining shared ownership. Logs include queue order, effective/base priority,
  core/ideal-core, affinity, immutable entry and entry argument. Foreign migration
  and status mutexes are only try-locked, and contended metadata is labeled.
  Queue/migration locks acquired by the diagnostic are released before logging.
- Request at most one sample of each observed front per round: at most twelve
  signal requests total. `SIGRTMIN+2` is used only if its observed user-facing
  **and raw libc** actions are both SIG_DFL. ART sigchain can expose a default
  user action while retaining a raw handler. If either layer is already owned
  or unavailable, preserve it and capture queues only. This follows the
  distinction already used by `common/signal.cpp::IsSigchainHooked` and
  [AOSP's signal interception](https://android.googlesource.com/platform/art/+/9fd68f6795eab7085986f80b1c4d00dc45a68485/sigchainlib/sigchain.cc).
- The existing guest-safe signal wrapper restores host TLS. The callback uses
  initialized raw `kernel::this_thread`, preserves errno, and copies ucontext
  PC, SP and all 31 GPRs plus guest/host mode into a lock-free mailbox. It performs
  no logging, allocation, locking, shared-pointer operations, memory inspection
  or context modification. No NCE trampoline or ThreadContext ABI is changed.
- Mailbox ownership is `Idle -> Requested -> Writing -> Ready -> Reading -> Idle`.
  Payload publication/consumption uses release/acquire ordering. Never recycle
  an unanswered request on a timeout. Cancel only after a definite unsent signal
  failure; retain strong target references while a response is outstanding.
- Responses remain associated with their original request round. Queue snapshots
  are sequential, and a target may move between observation and signal delivery;
  this is not an atomic global queue+PC snapshot. Late samples are drained after
  the three-round budget without requesting more signals.
- SA_RESTART reduces syscall disruption but cannot guarantee undisturbed timing
  for every syscall or ARM64 exclusive-access sequence. A diagnostic build
  advancing farther is not, by itself, proof of a functional fix.

All output is prefixed `[SCHED-STALL]`, including successful/failed signal
requests, pending responses, unavailable metadata and captured registers.

## Validation

The host scheduler gate runs with diagnostics OFF and ON. The enabled gate
compiles the production capture methods, signal callbacks and KThread request
method, with only context/signal-delivery boundaries supplied by Linux facades.
It covers the three-round budget, waiter filtering, read-only scheduling state,
all register values, errno, no handler logging, failed/unready targets,
contended foreign locks and user-facing/raw signal ownership. The mailbox gate tests delayed delivery, one outstanding
owner, cancellation, 10,000 concurrent publications and native POSIX signal
publication. Native POSIX tests do not execute Android's guest-TLS wrapper.

Passed locally: 25 scheduler cases with diagnostics OFF; 29 cases with diagnostics
ON under ASan/UBSan and TSan; and 50 full enabled UBSan repetitions (1,450 cases).
The mailbox gate's 10,000 publications and native signal test passed under both
sanitizer configurations. Unchanged submission and presentation host regressions
also passed. LeakSanitizer remains disabled for the environment's process-inspection
limitation; AddressSanitizer is active. Independent review found and resolved
the raw-vs-user signal ownership guard before publication, with a targeted test.

Gradle argument forwarding was executed against a wrapper, and the actual CMake
option block was configured with OFF/ON to verify generated compile definitions
and invalid waiter-ID rejection. Full ARM64 compilation remains the Actions
gate; it is not replaced by host fixtures. Gradle cache, CCache and the existing
GPU/compiler gates are preserved. The pre-existing unrelated host-GPU test
limitations from the preceding audit are not represented as newly passing.

## Device test

Use the new #362 ARM64 DevRelease artifact `strato-pr362-arm64-release.apk`.
It explicitly enables this bounded diagnostic; normal builds remain OFF.
Keep the driver, cache and save unchanged for the first comparison. Open MK11;
if it stalls, leave it in that state for about 15 seconds before exporting
`emulation.log`. Collect a progressing boot with the same settings if possible.
Capture before pausing/returning from the activity so swapchain lifecycle events
do not get confused with the original stall. The important records start with
`[SCHED-STALL]`; no stack or memory dump must be enabled manually.

PR #362 stays Draft and unmerged. No continuous Actions monitoring after
publication. Device PC/queue evidence and a consistent boot/gameplay result
remain pending.
