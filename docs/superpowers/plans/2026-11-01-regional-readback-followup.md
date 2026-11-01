# Regional readback follow-up implementation plan

> For agentic workers: use superpowers:executing-plans; implement and publish each validated stage in the existing PR416.

**Goal:** Remove demonstrated regional overhead and reduce waits on unrelated GPU writers without changing data authority.

**Architecture:** Keep staged memory, traps and Vulkan submissions. Separate the producer needed for a page's data from later buffer lifetime use only where submission metadata proves the distinction. Reject prefetch work with no identifiable consumer or demonstrated overlap.

**Tech stack:** C++20, Vulkan, existing Python host extraction harnesses, ASan/UBSan.

**Spec:** User's 2026-11-01 PR416 follow-up; mechanism and constraints in docs/performance/regional-buffer-coherence.md.

## Global constraints

- Same branch/PR416, preserve all local work, SSBO409/compiler and other Drafts.
- No hacks/relaxed fences, no speculative stale results, no Android diagnostic APKs.
- Commit/publish each validated stage; existing ARM64 DevRelease only. No Actions polling or merge.

## Review focus

Repeated binding must re-arm protection after real CPU ownership changes; boundary allocation failure must preserve old traps. Producer tracking must distinguish two writers to different pages, retain partial overlap dependencies, handle unknown/pending writes conservatively and keep full-cycle backing mutation/lifetime waits. Async publication must not overwrite newer CPU/GPU generations.

## Stage 1: stable GPU-dirty bindings

- [x] Test production trap syscalls: 2000 repeated bindings must need zero reprotections; observe baseline failure.
- [x] Test unchanged boundary insertion with allocation failure armed; observe redundant-reserve failure.
- [x] Skip unchanged ownership reprotection and avoid reserve without a split.
- [x] Run host/native Vulkan ASan/UBSan coherence, overlap, concurrency and failure suites.
- [x] Review and verify the stage; commit/publish with this checkpoint.

## Stage 2: regional producers

- [ ] Trace submission assignment, pending writers, coalescence and full-cycle lifetime requirements.
- [ ] Reproduce GPU A complete / different-page writer B pending: CPU A download must retain A only; B demand must wait B.
- [ ] Implement only if lifecycle, writer replacement and conservative fallback contracts can be proved.
- [ ] Run native Vulkan/ASan/UBSan plus submission/SSBO regressions, review and publish independently.

## Stage 3: early/async downloads

- [ ] Compare existing consumer callsites and executor completion with Eden generation-safe publication.
- [ ] Identify an exact advance demand and demonstrable independent CPU work before adding a queue or staging copy.
- [ ] If absent, document the missing horizon and why a worker memcpy/history-based prefetch is not justified; keep the safe demand path.
