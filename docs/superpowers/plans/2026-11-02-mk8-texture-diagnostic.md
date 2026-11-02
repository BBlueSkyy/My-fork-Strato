# MK8 texture diagnostic implementation plan

Goal: observe the initial black character selection with Free Guest enabled, without changing texture ownership or synchronization policy.
Base: master ab2011a42f629a6c458ade02fb4104a6d249cb69. Isolated diagnostic branch; PR #400 untouched.

- [x] Add a standalone bounded recorder with unit tests for phase separation, deduplication, overflow, contention and disabled behavior. Fixed capacity, no I/O or allocations on event recording, try-lock only. Distinguish policy ownership from declared RT/storage/copy writes; neither is proof that a conditional GPU instruction executed.
- [x] Hook texture creation/destruction, read bindings, declared write sites, state transitions, upload/readback, eviction, traps, alias registration/recovery and view resolution. Store guest CPU mapping addresses (not IOVA), formats, layout, mip/layer ranges, serials, fence identity and event provenance. Never read texture bytes from a trap.
- [x] Enable only for MK8 title 0100152000022000 with Free Guest on. Capture startup plus manual first-selection/race/returned-selection markers; save bounded CSV from a UI worker. Keep phase marking separate from file export. Record dropped/limited events explicitly; absence of a write in a truncated trace is inconclusive.
- [ ] Validate recorder and production hooks, review changes for policy/lock changes, preserve ARM64 DevRelease workflow, publish diagnostic Draft and request existing PR build. Runtime reproduction and phase comparison require the Moto G100.

Fallback Mini RenderDoc protocol: once lifecycle traces identify candidate IDs/mappings, target the same character/camera and one defective/correct frame. Compare sampled views/subresources/swizzles, RT/depth and shader/draw identities; read images only in an explicitly armed capture, after producers, without republishing guest bytes. Do not capture every texture or introduce persistent readbacks. No speculative fix.

## Execution ledger

Recorder, hooks, manual phases, export and analyzer implemented. Recorder/synchronization/submission/layout/alias/conditional-rendering/draw-texture host checks passed. The full Vulkan backing suite is blocked by missing Boost/Vulkan development headers and ICD; local Android Gradle distribution download is blocked. ARM64 DevRelease must be validated by existing PR CI.

Independent whole-branch review found phase-marker replacement, freeze/record races, missing bound-view metadata, null fallback visibility and incorrect storage swizzle. All were addressed; final remaining storage finding uses the identity mapping selected by the real storage binding. No emulation policy change was made. Host facade methods were adapted for diagnostic calls; the existing conditional facade also needed the current master InvalidateIndexBuffer API stub.

Ruling: the Mini RenderDoc fallback is a prepared target/capture protocol, not automatic GPU image readback. Candidate textures and the failing/correct presented frames must first come from the device trace. Without the game/device here, a specific captured frame or confirmed root cause cannot be claimed.
