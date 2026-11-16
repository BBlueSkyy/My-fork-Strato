# MK11: material-worker wait and stranded idle capacity

Parent: `466c934e513e0d70fbb7ebfd2df084bcdf1419f6`, existing Draft PR #362.
This continues the [bounded capture](mk11-stall-capture-20261116.md).

## Device evidence

| Capture | Three rounds (seconds in log) | Cached pipelines loaded | Async packages logged |
| --- | --- | --- | --- |
| `travado na tela de aviso .log` | 19.143, 20.424, 22.984 | 66 | 22 added, 19 completed |
| `depois da tela de aviso.log` | 44.192, 45.473, 48.033 | 0 | 54 added, 54 completed |

Both use Turnip 26.2.99. Cache contents differ, so this is not a controlled
cache A/B. The later capture reaches `BGND_MainMenuBackground` and unlocks
gamepads; it does not prove character selection or gameplay. The first has
three outstanding package log entries, which do not identify the blocking
dependency. Both contain audio minimum-latency error `-4`. Neither records a
native exception, device-lost error or process exit proving the separate crash.

All six T1 responses have identical guest PC `0x8003AD2B4`, LR `0x8003AD258`,
FP `0x198AAFFB50`, SP `0x198AAFFB30`, X9 `0x8088786A0` and W11 `1`.
At each queue capture, T1 is priority 35, C0, affinity `0x3`; T32 is priority
43, C0-only. C0 contains T1, T4 at priority 35, T32 and lower-priority work.
C1 is empty in all six rounds. C2/C3 are also empty except one contended C2
snapshot. Queues and asynchronous register samples remain sequential.

Log SHA-256:

- Warning: `e22ce5fdaef619151ac1b9dd1be20960c52fe9d1c019967c736ce6891ccd8c04`.
- Later: `9b002d70d96d4c8752aefea8e13cf9037242bf9b3597c7a68a97c0c0a21badbb`.

## Static identification

Previously supplied `MK11_ExeFS.zip` contains main, sdk and subsdk0. Main's
Build ID is `B87694BCC4E7C77E317111E82EDD8266` (remaining bytes zero);
SHA-256 `a9261805c012ecfcefe1dddceefe00ef9e22b08e0320715944381c3db423ef34`.
No game binaries are committed. Decompressed text and dynamic symbols, using
main text base `0x800008000`, identify:

- `main.text+0x3A51B0`, size `0x178`:
  `NrsDeferredMaterialParameterUpdate::WaitOnDeferredUpdateResourceProcessing()`.
- Guest PC `0x8003AD2B4` is `main.text+0x3A52B4`: `CMP W11,W8`.
- `0x8003AD2AC`: ARM `YIELD`; `0x8003AD2B0`: `LDAXR W11,[X9]`.
  The loop exits when the worker count is zero. W11 is one in all six samples;
  W8 is the compare-exchange sentinel `0x1B3D4A7F`.
- Symbol `NumActiveWorkers` is at guest `0x8088786A0`, exactly X9.
  Adjacent MITVs and MICs symbols also match X19/X20 in both logs.
- `KickDeferredUpdateResourceProcessing()` publishes the worker count and
  calls `NRSConcurrency::RunCodeHighPriority` for deferred work.

This material-worker wait differs from the earlier Wwise
GetSpeakerConfiguration / SleepThread(-1) wait. ARM YIELD is a processor
hint, not a SleepThread SVC. The loop already uses acquire loads; the earlier
unfenced-LDXR correction does not apply. No guest instruction is patched.

The evidence identifies the main thread's wait and stranded allowed capacity.
It does **not** identify which queued thread owns the remaining material job:
T32 triggering the capture does not prove ownership. It also does not prove
that this scheduler correction resolves every dependency or the later crash.

## Defect, design and implementation sequence

The original #362 migrates a running caller during SleepThread(-1). Timed
WaitSchedule balancing moves only its unscheduled caller. A pinned waiter
cannot free its core when the running front executes guest code without
another SVC, even when that front permits an idle core. This can persist
indefinitely. The regression `idle-front` reproduces it with production
WaitSchedule, Rotate, queues and condition variable: parent `466c934e` fails.

1. Reproduce the no-SVC topology before editing production code (failed).
2. Prefer the waiter's existing self-balancing. If still unscheduled, use its
   existing backoff to request idle-capacity reconsideration by its front.
3. Set a per-thread atomic request and reuse the yield signal; never change
   foreign queue membership. Try foreign migration/status locks without
   waiting. Reject unready/killed hosts and failed sends; deduplicate signals.
4. Consume the request on the recipient's own host, before WaitSchedule takes
   its core lock. Reuse MigrateToIdleCore, rechecking affinity, pause, source
   membership/demand and destination occupancy.
5. Verify stale observations, failed sends, existing signal handoff,
   contention, simultaneous waiters, equal priorities and pause/resume.
   A deterministic late-entry regression also caught a review issue: requests
   arriving during WaitSchedule must be acknowledged and reconsidered before
   return, with the core pointer/predicate refreshed after migration.

The atomic request distinguishes none, signal in flight, and host-delivered
retry. The existing host yield callback only acknowledges delivery with a
lock-free CAS. A delivered request that misses the final host-to-guest check
can be resent on the existing balancing backoff; it cannot permanently poison
signal deduplication. Failed sends roll back only the sender's pending state,
preserving concurrent acknowledgement/consumption. No callback allocates or
takes a new lock for this acknowledgement.
An unrelated pre-existing pendingYield has unknown delivery status, so borrowing
it is also marked retryable. The regression delivers that signal on HOST before
the idle request exists, then verifies that a later backoff can send a new one.
Consumption also synchronizes with the sender through the recipient's
migration mutex and acknowledges pendingYield under its queue lock. This
covers a request arriving after an earlier Rotate but before WaitSchedule
entry, including a sender still finishing signal delivery.

Unconditionally migrating every yielded thread would broaden unrelated
preemption and SleepThread(0) behavior. Direct foreign-front migration would
move a host that may still execute guest code. The explicit cooperative
request avoids both. Priorities, affinities and 0/-1/-2 operations remain;
an independent asynchronous capacity request may coincide with a yield.

There is no new timer, sleep, polling loop, priority override, PC/Title ID
check, service change or GPU modification. Cost is one atomic request check
at WaitSchedule entry and bounded inspection on an existing balancing timeout.

[Mesosphere's scheduler](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_scheduler.cpp)
UpdateHighestPriorityThreads also considers moving a scheduled front to an
idle permitted core when its source has other runnable work. It is a semantic
reference, not copied implementation: Strato's own host performs migration.
Global suggested queues remain outside this correction.

## Validation and limits

Validation completed before publication:

- Parent `466c934e`: `idle-front` fails with stranded pinned work.
- Diagnostic OFF: 36 scheduler cases with ASan/UBSan.
- Diagnostic ON: 40 cases with ASan/UBSan and TSan; 50 complete UBSan
  repetitions, 2,000 cases. Mailbox test: 10,000 concurrent publications,
  delayed delivery and native POSIX signal.
- GPU submission and swapchain presentation regressions passed; NCE
  acquire-lock matcher and wideLines compatibility tests passed.
- All three decompressed main segments match their NSO SHA-256 checksums;
  Python syntax and git diff whitespace checks passed.

The host runner
extracts full production methods, including TrySendSignal; only the pthread
delivery boundary is replaced. It cannot validate Android TLS signal wrappers
or execute the game's material worker. ASan uses `detect_leaks=0` due the
previously documented host limitation. The earlier unrelated host-suite
limitations in the [audit](mk11-pr362-20261116-audit.md) remain unchanged.

Diagnostics remain OFF by default and explicitly ON in the #362 test workflow.
Shader/GPU/texture/memory fixes, Gradle cache and CCache are preserved. Device
performance, consistent boot and the later crash require the new ARM64
DevRelease APK on Moto G100. Keep driver, cache and save unchanged; export a
new log after a stall and wait about 15 seconds before exiting. No merge; no
continuous Actions monitoring.
