# Buffer trap preparation dependencies

The prepare callback can block on the resource while a recorded execution owns
AllWrites. Production Buffer::unlock releases that restriction before unlocking.
The next callback attempt already allows CPU writes to a current mirror without
waiting on GPU readers. Recheck that predicate after acquiring the resource,
retaining the reader cycle for the eventual SynchronizeHost backing upload.

This does not bypass a GPU-dirty readback, an AllWrites restriction still active,
or a backing write dependency. No #411/#412 product changes are imported.

```sh
ASAN_OPTIONS=detect_leaks=0:handle_segv=0 python3 tests/buffer_trap_dependencies/run.py
# Expected failure against unchanged master (no working-tree mutations):
ASAN_OPTIONS=detect_leaks=0:handle_segv=0 BUFFER_BASELINE_REF=origin/master python3 tests/buffer_trap_dependencies/run.py
# Optional real Vulkan, loader/headers/ICD/Khronos validation layer required:
ASAN_OPTIONS=detect_leaks=0:handle_segv=0 VULKAN=1 python3 tests/buffer_trap_dependencies/run.py
```

The runner extracts current production callbacks, SyncGuest/Host, unlock,
TrapManager/IntervalMap and NCE's literal write dispatch. Test adapters were
selectively recovered from #413; no diagnostic hooks or read-routing changes
are used. Shared guest/mirror mappings and CPU faults are real Linux x86. The
host interfaces model allocation and fence completion; native mode waits a
real Vulkan fence and gates a GPU reader with a host-set VkEvent. It compares
every byte for the old consumer and a later consumer after the partial upload.
ASan/UBSan remain active; handle_segv=0 permits intentional page faults.
LeakSanitizer is unavailable in the restricted host.

Baseline deterministically fails because CPU store is blocked until the pending
GPU reader is released. Candidate removes that wait, keeps backing unchanged,
and still blocks its upload until the reader finishes. GPU-dirty preservation
still waits for its producer. Vulkan synchronization validation reports zero
errors on llvmpipe. No device performance gain is claimed. This proven class of
wait is not established as the cause of all 6780 GPU-dirty readbacks in Zelda.

## Focused attribution build (temporary diagnostic)

The second commit enables `-PbufferReadbackProbe=true` only for the PR DevRelease
build. The CMake option defaults OFF; normal builds compile the hooks to no-ops.
The optional reporter can be tested independently:

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tests/buffer_trap_dependencies/run_probe.py --deps app/libraries
ASAN_OPTIONS=detect_leaks=0:handle_segv=0 PROBE_ENABLED=0 python3 tests/buffer_trap_dependencies/run.py
ASAN_OPTIONS=detect_leaks=0:handle_segv=0 PROBE_ENABLED=1 python3 tests/buffer_trap_dependencies/run.py
```

`BufferReadback v2` emits one global line, up to four hot-resource lines and one
coverage line per five seconds, plus a final window. Each phase contains
`calls,bytes,completed_thread_ns`:

- `dirty_prepare`: FenceCycle waits selected while the resource is GPU-dirty.
- `current_prepare`: preparation waits selected with a current mirror but an
  outstanding restriction on backing writes.
- `readback_wait`: retained cycle waited by SynchronizeGuest itself, excluding
  an earlier preparation wait already completed.
- `upload_wait`: cycle waited before copying mirror contents into the backing.
- `download` / `upload`: bytes and CPU memcpy time, separately from waits.
- `hack`: unsynchronized buffer copies made by the existing hack, if enabled.
- `released`: first preparation recheck permitting a current-mirror write with
  a cycle retained. This is an opportunity count, **not measured time saved**;
  the retained cycle may already be complete.

Top IDs are ranked by dirty_prepare + readback_wait thread time, with each
buffer's size and all phase deltas. Calls from different threads can overlap;
never interpret these sums as time per frame, GPU execution time or simulation
speed. Times are recorded at scope completion (including failed wait attempts).
Atomic fields have approximate window boundaries; they are not a transactional
snapshot. Late bytes/time without a new call are still reported.

The fixed 512-row table admits only dirty/readback/hack resources, with at most
8 probes and no allocation, logging or extra locks in callbacks. Initial
uploads do not fill the table. Globals include events omitted from per-ID rows.
`dropped_records_all` counts failed dirty-row admission attempts cumulatively;
it excludes deliberately unadmitted upload/release events. Top-four truncation
also limits attribution and is separate from that counter. No row eviction or
reset occurs during execution. IDs are local to a BufferManager: restart the
app for every capture and do not correlate IDs across launches. Re-created
managers within one process are not supported for resource attribution.

The reporter uses synchronous logging on its own thread and is stopped/joined
before logger finalization. Tests cover concurrent exact counters, bounded
capacity, initial-upload admission, late fields and stop-before-finalization.
Native tests additionally gate a real GPU writer, verify the CPU store remains
blocked, then compare every readback byte after a partial store. A failed mocked
producer retains GPU authority and its cycle until a successful retry.

For the device capture, restart the app and use the same save/scene/driver for
90 seconds after reaching gameplay. Initially disable both readback hacks,
maximum GPU clocks and Free Guest; save the complete log including the v2 start
flags. Repeat the same scene with hacks enabled only for controlled comparison.
Record real elapsed time for the same gameplay action, presentation cadence and
any flicker separately. Do not infer FPS improvement from these diagnostics.
The log does not replace GPU timestamps, scheduler tracing, or alias/write-range
proof needed for a future producer-only or regional readback optimization.

## v3 sampled fence decomposition

The new build retains all v2 counters unsampled. `BufferReadback v3 phases` and
`v3 hot` report only one of every16 GPU-dirty prepare waits per admitted buffer.
Each value is `calls,0,completed_thread_ns`; no transferred bytes belong to these
phase counters. `samples` contains the total sampled wait time (including phases).
`chain`, `submit`, `mutex`, `driver`, `semaphore_tail` are disjoint **root-call**
segments. A predecessor's entire recursive wait belongs to `chain`; a semaphore
consumer's entire wait belongs to `semaphore_tail`. They are not also added as
root driver time. `mutex` may include another waiter's Vulkan wait while it owns
the completion mutex. It is not proof of avoidable CPU lock contention.

`unsubmitted` / `already_signalled` count sampled root states at entry; `nodes`
counts root and nested FenceCycle::Wait calls. Sampling is periodic and may align
with guest behavior: do not multiply by16 and present it as an exact whole-run
measurement. Metadata/table limits from v2 still apply. Small unclassified time
includes state checks, lifetime destruction and diagnostic overhead; phase sums
must not be added to `samples` or v2 `dirty_prepare`.

The production FenceCycle is used directly by these additional tests:

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tests/gpu_submission/run_trace.py
ASAN_OPTIONS=detect_leaks=0 SANITIZE=address,undefined python3 tests/gpu_submission/run.py
# With the loader, Vulkan-Hpp/headers, ICD and Khronos layer configured:
ASAN_OPTIONS=detect_leaks=0 VULKAN=1 python3 tests/gpu_submission/run_trace.py
```

Host tests gate each wait segment and verify cancellation and disjoint recursive
accounting. Native mode uses the production FenceCycle with a real VkEvent-gated
writer, copy and readback, comparing every byte with synchronization validation.
Host mutex/list interfaces are test facades; native Vulkan calls and FenceCycle
logic are real. No Android/Turnip performance result is inferred from llvmpipe.
