# XCI launch compatibility implementation plan

**Goal:** Correct reproducible loader defects on exact master 9935f133 and expose startup boundaries for Nicktoons without claiming device compatibility.

**Architecture:** Keep shared content resolution and existing NCA storage layers; align XCI with the existing deferred Program path. Validate malformed input before allocation and execution. Add bounded startup-only tracing, including synchronous Android logcat records, without changing native signal handlers.

**Spec:** User request in this session, 2026-10-29: XCI compatibility investigation, Nicktoons 0100668022DF4000, Moto G100/Android 12; no speculative firmware, GPU or NCE changes; Draft PR and ARM64 DevRelease.

## Global constraints
- Base master 9935f13349c00d732ae599f4f90866b5b79629f5; dedicated branch; no merge or changes to PR 400.
- Synthetic fixtures and generated test keys only; no secrets or game assets.
- Do not assert that host fixture success validates retail games.

## Review focus
- Valid XCI whose secure content extends beyond the root header hash extent.
- Unselected Program NCA with unavailable payload and selected invalid Program NCA.
- Short read despite a backing advertising a valid range.
- Exception before signal-handler installation or abrupt Android process death.
- Valid old NPDM/NSO and existing sparse/compressed/BKTR content.

## Tasks
- [ ] XCI/HFS0: reproduce shadowed selected content, root span, malformed table bounds and eager Program parsing; minimal corrections and real-loader host fixtures.
- [ ] Keys/NCA: reproduce missing EOF record, whitespace/comment handling and generation bounds; preserve errors and add fixtures.
- [ ] NSO: reproduce invalid LZ4, zero compressed size, short reads, RO diagnostic bounds and overflowing symbol extents; preserve ZBIC.
- [ ] NPDM: reproduce nested-extent/capability validation defects; retain recognized process semantics.
- [ ] Startup: log entry, completion, exception unwind and duration around resolution, GPU init, process, NPDM, ExeFS, NSO, VMM, mapping/patching and main thread. Test trace lifecycle. Preserve native signal ownership.
- [ ] Compare primary reference sources, document evidence and remaining format/service limitations.
- [ ] Run focused suites plus existing VFS/service regressions, review full diff, publish grouped commits and Draft PR, inspect existing DevRelease workflow through completion.

Execution is authorized by the user to proceed autonomously; no additional design approval is required. Independent loader domains use the parallel-agent skill; parent owns integration, tracing and build verification.
