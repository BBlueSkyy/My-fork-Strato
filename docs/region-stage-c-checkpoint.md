# Stage C activation checkpoint — 2026-11-04

This is a validated disabled increment of Draft #400 from `1db05677`, not a completed
Stage C release. All Stage A, B1a, GMMU/Unmap and preparatory commits are retained.
`MixedRegionAuthorityEnabled` remains **false**. The automatic host/Vulkan gate
uses a temporary enabled header; it never changes the production switch.
No game defect or device performance improvement is claimed. Sparse, B1b,
general physical allocation reuse and subsequent stages remain out of scope.

The published prerequisite record is [region-stage-c.md](region-stage-c.md).
The implementation plan is
[2026-11-03-region-stage-c-activation.md](superpowers/plans/2026-11-03-region-stage-c-activation.md).

## Coverage compared with published `1db05677`

| Area | Published baseline | Local implementation and proof | Remaining limit |
|---|---|---|---|
| Automatic Buffer → Image → Buffer | Raw transport helper only | Actual manager discovery, existing common ledger with typed writers, raw GOB repair, real Vulkan byte oracle | Temporary test activation only |
| Mixed GPU writer publication | No typed buffer writer | Prepare resources, retain node, validate mappings/readers/writer conflicts, then commit byte-range claim | Not a general executor transaction/rollback implementation |
| Partial writes and exclusive data | Transport tested separately | Sub-GOB buffer writes preserve the remaining sector/cell data; independent image and buffer producers retain exclusive bytes | Whole-GOB registration geometry; image-only Stage A classes unchanged |
| CPU publication | Whole staged-buffer backing publication | Own-cell masked publication; CPU read prepares only actual donors of the requested buffer, outside metadata/trap locks | Necessary image download still uses private whole-image staging and masked guest publication |
| Coalescing | No mixed identity migration | Resident versions, typed owners and delegate offsets migrate; replacement retained before ownership/link publication | Failure tests cover attachment, allocation, recording and retry boundaries, not arbitrary device loss |
| Already-published foreign bytes at coalesce | Stale buffer backing could overwrite guest | First-use regional source publishes only its own cells; real guest corruption reproduced at 2048/4096 bytes, repaired to 0/4096 | Full native Android allocator/record thread not emulated |
| Disjoint producer during CPU read | No mixed graph | Transitive unrelated image is neither waited nor downloaded | Structural discovery/merges can inspect involved components |
| Bound CBUF after remap without rebind | Selector cache refreshed, bound view could remain P | Bound slot retains translator/epoch/address/range; CPU and descriptor use refresh to Q; P→Q→P and nonresident recovery tests | No claim of B1b physical allocation identity |
| DMA / Inline / CBUF Load during remap | Physical spans or CPU write escaped publication lease | IOVA translation inside retry, bounded CPU mapping lease or one GPU publication node; exact old-P write reproduced then repaired | Fragmented DMA Copy retains the existing GMMU fallback; sparse/DMI GPU transport not admitted |
| Unsupported mixed overlap | No mixed authority | Explicit eligibility rejection before writers/nodes | **Release blocker:** normal unsupported overlap still throws; safe fallback/admission retirement is not implemented |

## Repaired review findings

1. An attached-buffer coalesce could link delegates to an unretained replacement
   when its callback retried or threw. Retention now precedes identity/link
   publication; failure preserves old graph, delegates, pending consumer and bytes.
2. Coalescing a GPU-dirty buffer could overwrite guest cells already published
   by an image with its stale whole backing. Regional sources use selective
   publication while preserving raw backing migration.
3. CPU publication had followed every resource in a transitive component. It
   now snapshots, acquires and revalidates only donors for the requested buffer.
4. Bound CBUFs retain their guest association independently of the selector
   cache and retranslate after a real mapping epoch change without rebinding.
5. After publication became selective, draining only the first buffer could
   leave another member's exclusive writer unpublished. The drain test
   reproduces `Cannot drain unpublished mixed buffer cells`; draining now
   publishes every participating buffer before validating/detaching the group.

## Explicit activation blocker

Automatic discovery currently collects every physical overlap. Registration
admits only staged buffers and native RGBA8 UNORM optimal 2D, one mip/layer/sample,
fully resident, with at most 128 complete GOBs per image. DMI, sRGB, partial edge
GOBs, nonresident mappings and other layouts are not admitted by this mixed path.
Their existing image-only handling is preserved while the release gate is off.

Enabling globally would make ordinary unsupported attachment throw. Returning
success or merely ignoring an unrepresentable peer could skip a valid foreign
writer. Draining an existing mixed group publishes its current writers, but
does not by itself order subsequent untracked buffer/image writes. Activation
therefore requires a tested fallback or per-component admission and retirement
protocol that retains future producer/consumer coherence, caches rejection and
preserves pending consumers. No sparse or B1b work is needed to establish this
boundary; it must not be bypassed to claim Stage C completion.

## Validation evidence retained from the checkpoint

Clang 18, lavapipe, Khronos synchronization validation; `ASAN_OPTIONS=detect_leaks=0`.
No Moto G100, Android DMI import or ARM64 APK execution is available on this host.

| Gate | Verified result |
|---|---|
| `gpu_buffer_image_sequence/run.py --automatic`, ASan+UBSan | Passed after coalesce/publication/dependency repairs, Free Guest OFF/ON and both format-list modes; `mixed-postreview-combined-asan.log` |
| `TEXMAN_AUTHORITY_PROBE=1 gpu_backing_promotion/run.py`, ASan+UBSan | A→B→reuse A **0/1024** stale texels, Free Guest OFF/ON and both format-list modes; `mixed-authority-regression-asan.log` |
| `gpu_region_executor/run.py`, ASan+UBSan | Preparation, cached replay, node allocation, conflicts, clear/render-pass rollback and retained query-end callbacks passed; `clear-report.md` records negative controls |
| Bound CBUF selector/cache/bind/CPU/descriptor tests, ASan+UBSan | P→Q→P without rebind, translator identity, nonresident/recovery, clamp and late lease rejection passed; `mapping-cbuf-full-green.log` |
| Master-provided SSBO gate on current binding bodies | 96 real binding paths passed; exact submodule headers supplied externally, no shader/compiler or workflow changes |
| GMMU generation, mapping identity, residency, texture sequence, framebuffer cache | Current gates passed during continuation |
| Submission, sampled layout/aliases, storage preflight, overlap materialization | Updated isolated facades for new APIs, then current exact-body gates passed; mixed branches throw in image-only facades rather than simulate success |

Do not interpret the unsupported-admission rejection tests as a successful
ordinary fallback. Do not reinterpret old logs as tests of later changed code.
The final mapped-entrypoint automatic suite passed with helper range checks, quiescent direct CPU branches, forced remaps during real reconciliation, partial/overlapping copies, CBUF Load offset validation and the multibuffer drain regression (`dma-mapping-final-full-green.log`). The CBUF selector/cache/bound-slot regression also passed (`dma-cbuf-selector-regression.log`). All of these used the existing real Vulkan host fixture; no Android imported-memory transport is certified.

A subsequent extra coalescing test initially expected the replacement lock to be released before executor submission. That expectation was wrong: retention intentionally transfers the lock to the executor. The corrected extra test remains local until its own run completes; it is not part of this validated publication.

The earlier full-production independent review produced C1/C2/I4, now covered by RED→GREEN data tests. A fresh review of the mapping delta was attempted but the reviewer stopped at the service usage limit before providing findings. The mapping delta received author review; independent follow-up remains pending.

## Performance evidence and limits

The standalone benchmark archives an exact commit, changes only a temporary test
gate, uses actual production methods and Vulkan copy/barrier hooks, and counts
host allocations. Timing builds use `-O2` separately from sanitizer correctness
runs. Repeated valid image preparations and repairs are measured against
`1db05677`; the mixed build additionally measures unshared buffer and cached mixed
buffer/image preparation. It pins only the measured control thread, reports
its CPU, and keeps the raw sample summaries in JSON. It is not a device FPS test.

An initial same-harness comparison found additional unshared image preparation
cost (37–48% in this microbenchmark). `AcquireRegionPublication` now takes only
the mapping lease when no regional resource participates, avoiding resource-set
construction and sorting. Functional executor/rollback tests passed after the
change. Final timing assessment remains separate from this correctness result.
Measured stable paths require zero allocations, copies and barriers; the mixed
benchmark also asserts no extra submissions, GPU waits, trap changes, image or
staging allocations and no group replacement. Structural changes may allocate;
reconciliation copies only stale represented regions, not a whole image repair.

No claim is made that host timings preserve Zelda, FighterZ, Odyssey or SpongeBob
FPS. Existing game defects remain the device baseline. Android memory, stability,
driver behavior and performance with Free Guest OFF/ON require device validation.


## Publication boundary

This increment preserves and publishes the verified shared-authority implementation with its release gate disabled, mapped DMA/Inline/CBUF corrections, multibuffer drain correction, exact-body regressions and benchmark harness. It is not an activation milestone. No new runtime diagnostics or CI-mandatory tests/workflows are added. A single branch update carries the organized commits; the existing ARM64 DevRelease workflow is used without monitoring its execution.
