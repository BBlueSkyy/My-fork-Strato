# Stage C automatic mixed authority activation

Base: 1db05677071ce64d6755b8a47a059f8c2a1c4ce5, existing Draft #400.
Spec: the user's complete Stage C activation request; docs/region-stage-c.md records the existing transport prerequisite and release gates.

Goal: automatic VkBuffer/VkImage authority transitions in real execution, preserving the existing image region ledger, B1a mapping leases, physical data and pending consumers. No new standalone transport-only milestone qualifies as activation.

Global constraints: preserve every previous commit, image-only fast path and existing shader compiler; no merge, #421 changes, game-specific fixes, sparse/B1b activation, global GPU waits, global per-draw scans, infinite retries or metadata-held resource/GPU/trap waits. Publish only validated changes to #400 and request one existing ARM64 DevRelease at the end without monitoring Actions. Unsupported classes must remain explicitly gated; do not claim general physical-allocation identity or untested device FPS.

Review focus: CPU whole-buffer readback clobbering foreign image-owned bytes; buffer merge delegate/owner lifetime; buffer writer claims before late descriptor failure; cached descriptors that bypass manager lookup; CPU writes after preparation; mixed writer conflicts in one operation; allocation/record/submit failures; stale mapping receipt reuse; stable resource allocation/lock/transfer overhead.

Tasks:

1. Recover canonical remote head and inspect local/remote/master deltas. Preserve the CI fixture follow-up. Record baseline host timing/operation counts on reproducible workloads.
2. Implement mixed regional registration, selective CPU publication, typed donor reconciliation, actual deferred GPU writer commit, B1a preflight, callbacks and coalesce migration as one cohesive release gate. Integrate producer and consumer paths (cached SSBO/texel/CBUF/vertex/index, DMA/inline copies, sampled/storage/RT images) before enabling registration. Reuse the existing 16-byte sector copy proof where eligible. Keep format/layout/DMI restrictions explicit; do not silently expose an unsafe path.
3. RED→GREEN production-body real Vulkan automatic coherence tests: Buffer→Image→Buffer, CPU round trip, partial overlaps/exclusive bytes, aliases, multiple producers, pending consumers, failures/cancel/retry, coalescing and mapping changes. Capture operation/transfer/barrier/wait/allocation counts and stable-path benchmark distributions before/after.
4. Run authority A→B→A (0/1024 OFF/ON), lock probes, GMMU/B1a and all affected essential regressions under Vulkan synchronization validation and ASan/UBSan. Review complete diff independently and fix material findings before publication.
5. Publish organized validated commits in the same Draft #400 and request exactly one ARM64 DevRelease via existing workflow. If an architectural requirement cannot be validated safely, document concrete evidence and leave that activation disabled instead of inventing completeness.

Decisions: extend existing authority and operation retry boundaries rather than adding a second conflicting physical authority graph. Allocation identity remains a live-mapping scope boundary, not a reason to reset valid old physical history on an IOVA change. Registration/migration may allocate on structural changes; stable unshared draws may not allocate or scan.
