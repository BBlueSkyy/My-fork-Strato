# Coherent resource hotpaths implementation plan

> Execute inline with test-driven-development and verification-before-completion.

**Goal:** reduce proven redundant CPU work and unnecessary GPU completion waits on current master, keeping correct emulation.
**Architecture:** preserve existing ownership, upload/readback, barrier and lifetime mechanisms. Reuse only independently demonstrated changes from #401 and the no-op layout correction from #400; do not import either branch wholesale. Each change remains independently revertible.
**Tech stack:** existing C++20/Vulkan, Python host runners, ARM64 Android DevRelease.
**Spec:** user request of 2026-11-01, “Investigar e implementar otimizações reais de CPU/GPU sem depender de hacks”.

## Constraints
- Base fc8eb519; preserve compiler a668a313 and all #409 contracts.
- No #405/#406 changes, per-game behavior, clocks/timer changes, skipped work, new settings or mandatory global waits.
- #400 remains independent and Draft; master does not contain its regional extension. Preserve current aliases and test optional compatibility against #400.
- Existing #401 fixtures prove CPU savings; device evidence did not recover Zelda FPS. Revalidation checks the new base; do not repeat historical game A/B.
- No device access: measure fixture costs, label them, provide same-scene Moto G100 acceptance. Confirm actual CI and artifact separately.

## Review focus
1. Borrowed/owned render-pass hashes and equality must agree, including depth and resolves.
2. Buffer early termination relies on disjoint live mappings; preserve reverse lock order and page-table path.
3. Same-layout requests must retain producer cycles, while real transitions and backing availability still synchronize.
4. Reuse must not publish stale GPU bytes or retire objects before fences; current readback/alias mechanisms stay intact.
5. Host timings do not establish Android frame rate, GPU time, governor behavior or game simulation speed.

## Tasks
- [x] Reuse #401 tests first; confirm RED on current master. Port missing-resolve hash and bounded predecessor scan separately; run sanitizers and controlled CPU benchmarks using the same fixtures.
- [x] Extend cache collision/depth equivalence tests before fixing any newly exposed predicate mismatch.
- [x] Extract production TransitionLayout into a dedicated fixture; demonstrate redundant wait before fix, then retain cycle on no-op; test changed layouts, pending submissions, backing publication, failure/retry and real Vulkan copy/readback with retained lifetime.
- [x] Run affected cache/buffer/submission/texture/SSBO suites and relevant real Vulkan tests. Check #400 integration separately without importing its implementation.
- [x] Record audit of buffer/readback/NCE/submission/pacing, Eden/Ryujinx references, measured savings and remaining uncertainty. Remove temporary instrumentation.
- [ ] Independent whole-branch review completed with no product blockers; final publication/build verification remains. Push new branch and create dedicated Draft PR, trigger existing ARM64 DevRelease, confirm success and nonempty ARM64 APK. No merge.
