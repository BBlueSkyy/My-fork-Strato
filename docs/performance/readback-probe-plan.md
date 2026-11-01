# Readback critical-path probe — implementation plan

Base fc8eb519 (master + #409). Preserve all old worktrees, commits and Drafts.
Spec: user's 2026-11-01 request, including diagnostic fallback when device
measurement is missing. #411 and #412 had no perceptible Zelda benefit.

Approach: diagnostic-only build; no new synchronization optimization until a
device trace demonstrates its target. Selectively reuse previous bounded atomic
counter design and the #412 classifier as an observation oracle, without porting
its NCE routing or TrapManager changes. No shader/SSBO experiments repeated.

- [x] Test and implement bounded counters, latency histograms and recent slow
  samples. No allocation, logging, blocking locks or data hashing in callbacks.
  Report completed span thread-ns explicitly, never as frame wall time.
- [x] Instrument original buffer/texture callbacks, actual copies, mandatory
  waits vs hack branches, fault eligibility, reprotection/invalidation, alias
  reconciliation, submission, driver waits and real-clock present intervals.
  Keep scopes/observations separate from decisions, locks and resource lifetime.
- [x] Reporter every 5 seconds outside critical paths, settings snapshot and
  read-only optional Adreno frequency sample. Missing permission is explicit;
  point samples do not establish ramp-up or GPU execution time.
- [x] Host ASan/UBSan, negative controls, no-op/off parity, native Vulkan and
  existing submission/alias regressions. Record exact facade boundaries.
- [ ] Independent review, publish new Draft with diagnostic label; ARM64
  DevRelease using existing workflow, verify successful build and APK. No merge.

Review focus: nested wait double counting; unknown ESR fallback; signal safety;
reporter races/bounds; exceptions/cancel/destruction; diagnostics enabled vs off.
Ruling: choose explicit diagnostic fallback authorized by user, not another
synthetic optimization advertised as a Zelda fix. Need A/B on Moto G100 to
identify whether bytes, producer waits, readback transfer, submissions or pacing
dominate. No optimized C exists yet; do not invent its result.

Review fixes: synchronous reporter logging (ASan TLS UAF RED→GREEN); one OS::Execute owner and Stop before logger Finalize; emit late atomic fields (RED→GREEN); actual Texture producer wait label (RED→GREEN). No deferred reviewer minors. Publication/build/APK remain pending.
