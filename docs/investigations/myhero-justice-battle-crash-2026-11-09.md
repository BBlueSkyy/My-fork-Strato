# My Hero One's Justice: battle-entry native crash probe (Android)

Status: diagnostic only. Do not merge before evaluating capture and removing
the probe. This work is independent of the sparse texture fallback in PR #373.

## Evidence and open questions

Android bugreport from 2026-11-09: the emulation process received SIGSEGV.
The KGSL driver separately reported GPU write translation faults against a
previously freed range. Those faults followed the app leaving the foreground,
so their timing does **not** establish that a Vulkan resource release caused
the guest battle-entry crash. No symbolized crashing stack was available.

## Activation and output

The probe is enabled only when EmulationActivity launches title ID
`010039000B68E000` (case insensitive). Other games incur only the cost of
a disabled atomic check. No global signal handler is installed, and the NCE
trap handling and memory-management semantics are unchanged.

Output: alongside `logs/emulation.log`, a new file
`logs/myhero-battle-probe.txt` in the app's public files directory.
It is truncated when a new My Hero session starts.

To reproduce:

1. Install the instrumented DevRelease APK from this draft PR.
2. Start My Hero One's Justice and attempt to load a battle once.
3. After it closes, save **myhero-battle-probe.txt** immediately and the
   Android bugreport taken near the crash. Include `emulation.log` only if
   it has new relevant data; don't repeat attempts without checking capture.
4. If the probe file is missing, check that the ROM title ID in the UI is
   `010039000B68E000`, and verify that app file storage is writable.

## What the probe records

- Android process ID, initialization milestones (loader/GPU/guest), surface
  attach/detach, explicit stop requests, and native catch paths.
- Vulkan command-record and submit handoffs, with execution tag and cycle
  pointer; VMA buffer/image allocations and frees, with allocation identifiers
  and allocated sizes, and texture synchronization/destruction markers.
- Fixed-size (512-entry) lock-free memory ring; stores only the latest events
  without allocating or blocking during normal GPU operations.
- Every three seconds, up to 300 snapshots with the most recent 128 events,
  persisted through append-only writes; a fatal signal or normal exit also
  triggers a snapshot. These bounds prevent unlimited diagnostic growth.
- For non-recovered host or guest signals, signal number, fault address, PC,
  LR, SP (AArch64) are written through the existing signal handling path,
  with no heap allocation, formatting library, or stack walking in the probe.

Event fields `a` and `b` are hexadecimal. Resource allocation events use
`a` as the VMA allocation identity and `b` as size when known; command
events use execution tag and command-node count / cycle pointer. The `ms`
field is `CLOCK_MONOTONIC` since device boot, not wall-clock time.

**Limitations:** Vulkan driver-internal allocations and direct imported
buffers are not tracked. An allocation/free pair alone does not prove a GPU
use-after-free: it must be correlated with submitted work and fence completion.
A fatal signal dump captures register addresses, not a fully symbolized stack;
Android tombstones and build symbols are still needed for exact call stacks.
GPU faults that never reach a userspace signal callback depend on the last
periodic snapshot.

## Removal

Remove the dedicated header and its callsites after the cause is established.
Keep any eventual fix in a separate, narrowly scoped PR.
