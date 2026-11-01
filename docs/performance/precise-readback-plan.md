# Precise readback / NCE implementation plan

Base fc8eb519 (master + #409), independent of Drafts #400/#405/#406/#411.
User requests implementation and publication, not another approval round.

Recovered work: #411 and its local checkpoints remain clean/preserved. Prior
cycle-order diagnosis already has an observational ESR parser and demonstrates
read-as-write causing unchanged uploads; reuse it, do not repeat SSBO history.
The user's ~30 FPS was hacks ON; #411 did not improve Zelda perceptibly.

Hypothesis to test: unconditional NCE write routing makes CPU reads discard
write protection, mark GPU resources CPU-dirty and later upload unchanged data.
Existing unused read-path reprotection must be repaired before enabling reads.
This is a memory ownership transition change, not a render/cache optimization.

1. RED/GREEN actual TrapManager+IntervalMap page protections: a GPU readback
   must keep future CPU writes trapped, including overlapping/retried callbacks.
2. RED/GREEN both actual NCE trap call expressions. Selectively recover/harden
   the existing ESR parser: bounded records, permission data aborts only, reject
   malformed/missing/ambiguous/maintenance syndromes, preserve legacy write on
   unknown. No PC dereference, allocation, logging or new runtime option.
3. Execute production staged buffer callbacks and sync bodies with real shared
   mirror/guest mappings. Compare legacy-safe/hack/precise routing for copied
   bytes and required waits, partial/full writes, retries, concurrent producers,
   replacement/lifetime and aliases. Do not label synthetic transfer savings FPS.
4. Native Vulkan upload/GPU-write/readback and mandatory dependency tests under
   synchronization validation; ASan/UBSan on testable components. Use test-only
   counters, no per-fault production diagnostics. Review independent of author.
5. Document hack bypasses, unavoidable readback costs, uncertainty about actual
   device frequency, deferred region/async mechanisms and controlled A/B/C device
   protocol. Publish Draft and confirm ARM64 DevRelease APK; no merge.

Acceptance: correct CPU bytes AND retained write traps, no upload for a pure CPU
read with unchanged guest memory, real writes still preserve untouched bytes and
upload normally. No new global wait or data elision based on guesses/equality.

A/B/C game comparison requires the Moto G100 and cannot be run here. Ship a clean
candidate APK and exact test conditions; do not claim normal speed or 30 FPS.
