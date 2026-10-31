# Rejected megabuffer observation experiment implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Test whether counting rejected oversized views causes the full-buffer copies observed in Zelda's Compute SSBOs.
**Architecture:** On abf26faa (master b8593d8b plus temporary diagnostics), stop adding a rejected view's size to the megabuffer copied-size heuristic. Keep every memory range, validation, fallback, compiler and Vulkan command unchanged. This is a controlled experiment, not a final correction or FPS claim.
**Tech Stack:** C++20 host tests with production method bodies and byte-copy allocator fixture; existing ARM64 DevRelease workflow.
**Spec:** User's ongoing PR294 isolation requirements and attached emulation.log (SHA256 94f9d26e903546dfc51efd069831175abe300265ed3953c58413ac82df68a058).

## Global constraints

No merge, no #405/#406, no options, no reduced SSBO limits or compiler rollback. Keep diagnostics separate from an eventual master-based final fix. One build, no monitoring. Marvel and Zelda device validation remains mandatory.

## Review focus

Repeated rejected 8 MiB views; a 4-byte view after a rejected request; GPU-dirty/direct eligibility; real small-copy promotion and cross-execution reuse; byte contents before/after sequenced mutation.

## Task 1: Reproduce, isolate, publish one experimental variant

- [x] Add tests extracting real TryMegaBufferView and validation/reset methods; rejected large views followed by a four-byte view must not copy the entire 8,470,528-byte mirror.
- [x] Run against parent, require assertion failure caused by full-copy promotion (not compilation error).
- [x] Remove only `megaBufferViewAccumulatedSize += size` from the existing oversized rejection branch; retain size threshold and every binding fallback.
- [x] Run targeted tests, existing SSBO binding differential tests and ASan/UBSan; compare production diff with parent.
- [x] Document measured shader hashes, Phi failure, physical buffer 172 costs and the remaining causal/device limitations; review the change.
- [ ] Publish separate experimental branch/one commit, adapt existing DevRelease workflow provenance/name, link and comment in #404. Do not monitor Actions or claim APK success.
