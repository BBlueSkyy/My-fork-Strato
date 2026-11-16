# Host scheduler regressions

Run `python3 tests/kernel_scheduler/run.py` from any directory. Use
`STRESS_RUNS=50` for repeated full-suite runs after one compilation,
`SANITIZE=address,undefined ASAN_OPTIONS=detect_leaks=0` for memory checks,
or `SCHEDULER_BASELINE_REF=<git-ref>` to compile the original production
methods as negative controls. An optional case name selects one scenario.

The runner extracts complete production method bodies and uses the real
adaptive condition variable. Linux facades supply KThread guest/signal/timer
boundaries, DeviceState TLS, logging, and the spin instruction. Test-only
pthread wrappers detect unmatched mutex unlocks deterministically.
Source/candidate publication tests hold the real queue lock while starting a
selector, then publish runnable work before releasing that lock. Concurrent
publishers additionally exercise balancing during queue insertion/removal.
`SANITIZE=thread` enables ThreadSanitizer; this does not reproduce Android
signals or prove all guest/ARM64 interleavings safe.
The production scheduler header is used with access controls relaxed only
in the temporary build to inspect queues. No test API is added to production.

`idle-front` reproduces stranded capacity when a busy guest front makes no SVC:
the pinned waiter's existing timeout must request a cooperative migration.
`SCHEDULER_BASELINE_REF=466c934e python3 tests/kernel_scheduler/run.py idle-front`
is the failing negative control. Ten related cases cover affinity/occupancy,
stale demand, pause/resume, equal priorities, failed/unready delivery,
nonblocking foreign locks, simultaneous requests and late host delivery/retry.
TrySendSignal and HostSignalHandler are extracted from production; pthread
delivery and the native queue-lock rendezvous are test boundaries. See
`docs/diagnostics/mk11-material-worker-stall-20261116.md` for device evidence.

See `docs/diagnostics/mk11-scheduler-yield-progress.md` for evidence,
reference comparisons, proof boundaries and device-validation limitations.

`python3 tests/kernel_scheduler/run_diagnostics.py` additionally tests the
lock-free context mailbox and compiles the real scheduler hooks with diagnostics
enabled and waiter filter T32. `SANITIZE`, `ASAN_OPTIONS` and `STRESS_RUNS` also
apply. The production KThread signal request and both diagnostic callbacks are
extracted; Linux facades provide AArch64 register-context and signal delivery.
The mailbox test also uses an actual POSIX signal, without Android guest TLS.
See `docs/diagnostics/mk11-stall-capture-20261116.md` for activation, bounded
capture behavior and the remaining device-validation limits.
