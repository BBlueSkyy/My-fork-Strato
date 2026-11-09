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
- For non-recovered host or guest signals, signal number, thread ID,
  fault address, PC, LR, SP (AArch64) are written through the existing signal
  handling path, with no heap allocation, formatting library, or stack walking
  in the probe. Startup also logs the libskyline ASLR module base.
- If the existing signal exception handler successfully unwinds, up to 48
  already-collected frames are persisted as `signal_frame` events outside the
  signal context; their `b` field is the frame address and `a` the frame index.

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

## Captured battle crash (first device probe)

The first capture recorded an **unhandled guest** `SIGSEGV` on thread
20127 with `fault=0x2`, `pc=0x8000a56bc`, `lr=0x801d93068`,
and `sp=0x19a8f36e60`. This is consistent with a near-null pointer
access while executing NCE guest code. It does not, by itself, identify
the instruction's NSO, source-level function, or the source of the null
pointer.

The final snapshot contained 127 buffer frees and a subsequent 16 MiB
buffer allocation, ending ~39 ms before the guest signal. These are VMA
resource identity events, **not** evidence that a freed buffer was
subsequently used. No Vulkan device-loss result or fatal host exception
was recorded by this probe.

**Next capture (same PR, additional targeted evidence):** the probe now
writes executable text mappings (`EXECUTABLE` entries), all 31 AArch64
register values and PSTATE during an unrecovered fault, plus the NCE's
already-computed guest call stack (capped at 4 KiB). Use the exact same
reproduction. Do not implement a resource-lifetime fix until the
instruction and causality are established.

## Second device capture (2026-11-09, same game, Turnip 24.0.99)

A more specific **guest Unreal Engine 4 assertion** was recorded first, on
`FAsyncLoadingThread` (HOS-28) at 56.483 seconds:

```text
AsyncLoading.cpp:3481
Texture2D /Game/GUI/Ingame/Hud/Texture/T_ui_hud_eff_PlusUltra_Gage_Count_l
Serial size mismatch: Got 230770, Expected 1442171
```

The first host `SIGINT` appears at 56.486 seconds, just after this assertion.
A host `SIGSEGV` appears at ~56.504 seconds, during shutdown. This run
**does not** show an unhandled guest SIGSEGV at the earlier PC. The ordering
supports the UE4 assertion as the proximate failure; the later native signals
must not be treated as evidence for a GPU lifetime bug.

For UE4 serial size checks, `Got` is the number of bytes consumed during
object serialization, versus `Expected` from export metadata. It does not
by itself prove a short RomFS read, nor a corrupt NSP. Possible contributors
remain malformed data, incorrect file bytes, guest memory corruption, or
different serialized object behavior; none is proven.

The base game loads without DLC/update, and the program RomFS is IVFC
(no indirect or compressed overlay reported in this run).

### Diagnostic change for next capture

Record samples (1 in 64, max 1500) of `.pak` file reads at the FSP-SRV
`IFile::Read` boundary. Each `PAK_READ` line includes the file path,
offset, requested and actual lengths, error status, and FNV-1a of the first
up-to-4 KiB returned. All failed reads are recorded. This does not change
the file data, read count, return values, game behavior, or cache policy.
It can reveal short reads or nondeterministic data for a repeated range but
is **not** a complete Pak verification.

The diagnostic signal path now excludes SIGINT from `FATAL_SIGNAL` reports
and names signal-context registers without assuming they are guest registers.

Recommended controlled follow-up is an identical battle on Strato with this
new APK; a same-content Eden result would help distinguish an image-specific
problem from Strato behavior. Keep GPU options constant initially.
