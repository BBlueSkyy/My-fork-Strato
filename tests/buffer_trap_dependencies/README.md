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
