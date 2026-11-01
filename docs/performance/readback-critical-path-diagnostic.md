# Device measurement after #411 and #412

Master base: fc8eb5196c198b281dce6bb3d9a9126b11ac5d4a (#409 merged).
Compiler gitlink: a668a31370f1739fdbf5375cad3e171d0fbd0601, unchanged.
All existing worktrees/checkpoints preserved; #400/#405/#406/#411/#412 remain
independent. Only the #412 **classifier and test adapters** are deliberately
reused for observation and regression coverage, not its routing/cache changes.

The user's Moto G100 result is authoritative: #411 and #412 brought no
perceptible Zelda improvement, while readback hacks still improve performance.
The approximately 30 FPS observations were **hacks ON**. Local synthetic savings
do not identify the dominant device cost, establish simulation speed or prove
that #412 receives eligible ESR metadata on this kernel.

## What is established, and what still needs device evidence

Master's buffer hack skips synchronized resource/fence completion before
backing-to-mirror memcpy. The texture hack can skip the entire GPU readback,
including producer completion, staging copy submission/completion and conversion
back to guest layout. Those bypasses can consume stale data. No safe fix can
copy that behavior when a real CPU consumer needs unfinished GPU bytes.

The existing safe texture path first waits its producer, submits a download and
then waits that transfer. A CPU waiter may instead block on FenceCycle's mutex
while the background waiter holds it inside vkWaitForFences. These paths are
now measured separately, without changing their dependencies. No device
measurement yet establishes whether producer GPU work, submission delay,
readback transfer, conversion, broad resource scope or presentation dominates.

The diagnostic's NCE observer invokes exactly #412's bounded ESR classifier on
the real signal record and counts eligible loads, known stores and conservative
fallbacks. Actual TrapHandler dispatch remains the master's literal `true`.
Thus it can answer **whether #412 would recognize the captured faults**, not
claim it changed routing in this APK or that the earlier tested APK did so.
Absent ESR / unsupported metadata is visible. Rejected raw syndromes can guide
a later compatibility investigation without decoding arbitrary instructions.

Region tracking is not added. CPU faults alone do not identify a full store
footprint, and GPU buffer/texture aliases need shared authority before partial
preservation can be skipped. Asynchronous readback is not added without an
observed overlap opportunity: moving a mandatory wait is not evidence of a gain.
The clean structural correction remains contingent on the resulting device log.
This Draft is diagnostic-only, not a performance-fix claim.

## New measurements and limits

See tests/readback_probe/README.md for exact schema and commands. Measurements
include original buffer/texture read/write callbacks, copy bytes, hack bypasses,
coarse dirty transitions, trap retries/reprotection/invalidation, alias
reconciliation, producer/transfer waits, FenceCycle submission/mutex/driver
waits, queue mutex/submission, executor submission, presentation API cadence and
backpressure. No production buffer hashes or per-fault log spam.

One reporter per OS::Execute writes at five-second intervals and final stop,
with settings snapshot and optional read-only GPU-frequency point sample.
Atomic counters and histograms are bounded; a 128-slot recent-slow ring and
64-thread attribution registry expose overflow. Output is synchronized only on
the reporter, outside emulation resource locks. It is stopped before logger
finalization and also on exception unwind. Diagnostics default OFF in ordinary
builds; this branch's existing ARM64 DevRelease workflow enables them explicitly.

Counters are completed inclusive thread spans, with approximate atomic snapshot
boundaries. They are not wall time per frame or GPU execution duration. Global
background waiter time is not all CPU time delaying the guest. Per-thread trap
time, resource kind and overlapping slow-cycle samples distinguish contexts.
Presentation return/request cadence is not display completion or game simulation.
GPU timestamp queries and a governor trace are not installed. A failed sysfs
read reports unavailable; a point frequency is not evidence of ramp-up behavior.

## Local validation

- ASan/UBSan core under eight concurrent writers: exact 8000 calls / 32768000
  bytes / 16000000 recorded ns; independent nested transfers retained, nested
  fence roots deduplicated, end-once and exception scopes, thread overflow,
  eligible PROT_NONE translation and permission faults and conservative unknowns.
- Real CPU faults/shared guest mirror + exact production NCE observation/dispatch,
  TrapManager/IntervalMap and staged Buffer bodies: safe and negative-hack
  outcomes unchanged, required wait retained, readback bytes exact, partial
  stores preserve other bytes and future write trapping is rearmed.
- Native Vulkan synchronization validation, diagnostic ON and OFF: 100 real
  fill -> mandatory fenced readback -> partial CPU store -> GPU copy cycles
  each; every byte compared, zero validation errors, no waits omitted.
- Native texture focused regressions with diagnostics ON: real images,
  Free Guest ON/OFF, callbacks, aliases, contention, registration after readback
  and failed readback recovery; zero validation errors. Unrelated historical
  full-suite fixtures are not claimed green.
- Existing production FenceCycle/SubmitInternal regression suite under ASan/UBSan,
  diagnostics ON/OFF: submission vs completion, pending cycles, cancellation,
  predecessor lifetime/chaining, direct/staged dependencies.
- Reporter uses actual fmt and logger API in tests; session deltas, prompt stop,
  final report before logger death, exceptions and counters with late atomic
  fields are covered. Production Texture::WaitOnFence extracted into a host
  test checks real wait classification, failed dependency retention and retry.

An independent review found four issues in the diagnostic itself. They were
fixed before publication: asynchronous logger records could retain reporter
TLS (reproduced as ASan heap-use-after-free), reporter lifetime ended after
logger finalization / was per channel, a calls-only snapshot predicate lost
late byte/time fields (deterministic failure), and ordinary texture producer
waits lacked their label (production-method test failed before instrumentation).
Reporter is now one OS execution owner with explicit Stop before Finalize and
synchronous reporter logging; every changed counter field is emitted, and the
texture wait label covers its actual wait. No emulation synchronization was
weakened to fix these issues.

Test interfaces/facades and synthetic ARM64 metadata are documented. Host
Vulkan is llvmpipe, not Adreno/Turnip; LeakSanitizer is unavailable due /proc
restrictions. No games were run here and no FPS/simulation improvement is claimed.

## Selective reference mechanisms

Eden #4490 flushes GPU-owned buffer bytes before NCE invalidation; the principle
is preserving data authority before a CPU retry. Its invalidation contract and
subsequent revert discussion mean that inserting an equivalent option in Strato
would not automatically be correct or faster.
https://git.eden-emu.dev/eden-emu/eden/pulls/4490

Ryujinx's pinned Buffer.cs uses granular modification queries, excludes GPU
modified ranges from CPU uploads and waits for relevant modified ranges during
external flush. Those mechanisms rely on its range ownership / syncpoint model.
They support measuring the consumed range and relevant producer, not skipping
Strato's current whole-resource preservation without that infrastructure.
https://www.git.axenov.dev/Museum/ryujinx/src/commit/a6a67a2b7add9a9dc8c4f0bab730957b8ebaf6e8/Ryujinx.Graphics.Gpu/Memory/Buffer.cs

No emulator code is blindly ported. Historical shader/SSBO A/B experiments are
not repeated. #409 and the modern compiler are retained.

## Moto G100 capture protocol

1. Driver Turnip 24.0.99, Zelda LA 1.0.0, identical save/scene/resolution/dock and
   shader cache. No 60-FPS cheat. Start at similar temperature. Enable Info log
   level; do not enable a heavier Debug/trace mode just for this probe.
2. First confirm this diagnostic with both readback hacks OFF / max clocks OFF /
   Free Guest OFF behaves similarly to the current master APK. This controls
   instrumentation perturbation; host overhead is not a device-overhead result.
3. A: fresh launch, all those options OFF; reach the chosen scene, warm up shader
   compilation, record about 60 seconds of steady scene/progress. Export the
   complete emulation log and note real duration, FPS and progress/slowdown.
4. B: fresh launch, same scene/settings, enable Fast GPU Readback and record the
   exact Fast Readback Writes setting used in the better-performing test.
   Leave max clocks / Free Guest OFF. Repeat and export a separate log. The
   startup banner verifies the configuration captured at launch.
5. If B's write setting is unknown, compare writes OFF vs ON separately rather
   than attributing both to one switch. Change settings between launches, not
   mid-run: the reporter snapshots them at start. Alternate A/B if repeating.
6. Optional clock isolation: repeat A with Force Maximum GPU Clocks ON, all other
   conditions fixed. Treat that as evidence, not a solution. Capture video with
   an external real-time reference for simulation speed; requested timestamps
   and FPS alone cannot establish it.

These A/B captures identify which cost the hack bypasses on the actual device.
There is no optimized C yet. A later C must preserve required bytes and compare
against both controls. Only if the log shows a concrete safe target should a
regional/async structural change be promoted as a correction.

For this observation-only build, Marvel (no magenta), SpongeBob (boot/menu) and
Sparking! ZERO remain useful regression checks. Free Guest ON/OFF is validated
locally; keep it OFF in the initial Zelda comparison to isolate effects.

Signal/TLS boundary: Android uses the existing NDK r27 / minSdk29 native ELF TLS configuration. Probe TLS fields have constant initialization and no destructors; callback code makes no explicit allocator calls. First access can still involve Bionic TLS runtime initialization, so host tests do not prove zero Android TLS overhead. Existing guest thread startup calls AsyncLogger::UpdateTag before entering NCE (same native module TLS). Warm-up/device overhead control remains necessary.
