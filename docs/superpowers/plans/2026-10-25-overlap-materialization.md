# Overlap materialization producer boundary

Continuation: PR #400 (e00e9371), after performance-first commit 9babcf1a; original master base 9935f133 unchanged. Deliver a tested first correction for a new divergence: FindOrCreate's partial-overlap fallback invokes SynchronizeGuest without the required Texture lock, and may encode an old backing while the current producer is still an executor node. The resource cycle is assigned at submission, so waiting on its old cycle is insufficient.

1. RED: compile the existing fallback against a pending producer. Demonstrate the new texture would consume old bytes.
2. Before readback, detect current-executor prepared-write receipts and unwind to the existing submit/retry boundary. Hold resource/state locks for publication, preserving retry state if this boundary's readback fails. Leave SynchronizeGuest/CopyToGuest (#392) and presentation/semaphores (#400) unchanged.
3. Selectively adapt the storage-image descriptor-preflight mechanism from #396, a prerequisite for retrying later texture resolution without phantom write claims. Test real GetImageBinding and the allocation/commit tails, full graphics/quick graphics/compute, failure and success.
4. Re-run focused production tests and essential regressions. Review independently. Append separate commits to existing Draft #400, targeting master. No new branch or PR. Workflow unchanged; no Actions monitoring or merge.

This stage is not the persistent physical-region ledger from #393. It must not claim the live A→B→A/cached-descriptor case is fixed, nor that UE4 graphics or FighterZ FPS are corrected. The later graph requires complete draw/dispatch writer preflight and must avoid #393's bitmap-footprint construction and broad hot-path work. Known region-authority/eviction/remap cases remain pending rather than being hidden by a partial port.

Independent review ruling: use atomic prepared-write receipts rather than historical serial mismatches. Clear at submission and unlock to prevent repeated retries for reconciled aliases. The real Vulkan native materialization target passes; the #393 live-reuse oracle remains 1024 stale texels and is explicitly pending.
