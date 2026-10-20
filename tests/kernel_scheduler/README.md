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
The production scheduler header is used with access controls relaxed only
in the temporary build to inspect queues. No test API is added to production.

See `docs/diagnostics/mk11-scheduler-yield-progress.md` for evidence,
reference comparisons, proof boundaries and device-validation limitations.
