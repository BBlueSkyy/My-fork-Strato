# Resource sequencing stage 1 implementation plan

Goal: fix four new, bounded CPU/GPU resource-ordering failures on master 9935f133.
Architecture: retain the existing per-texture lock, dirty-state machine, submission fences and semaphore handoff. Do not add a parallel global resource graph.

User-authorized scope: implement and publish a Draft from master; no merge, Actions monitoring, shader changes, repeated #392/#399 readback fixes or mandatory CI tests.

1. Compile the actual upload methods against gated host resource interfaces. Show a failed upload leaves an ordinary texture Clean/GpuDirty and suppresses the retry. Cover OFF/ON eviction, sampled/writable images, inline/standalone, prior submission, record/submit/attachment failures and CPU writes during staging.
2. Retain CPU authority after failed uploads. Use pre-submit lifetime attachment and layout rollback for ordinary standalone uploads, as already done for exact aliases. Keep successful upload behavior and the asynchronous executor path.
3. Compile the actual texture-copy method. Show an unfenced source remains unfenced during a pending GPU read; also cover a second copy after a semaphore has already been consumed. Track the new read cycle on both participating textures without adding GPU waits. Preserve failure state.
4. Materialize fragmented GPFIFO commands only after the existing dirty-producer synchronization. Test clean/dirty, contiguous/fragmented, deferred macros and failed flush retry. Keep existing wait semantics.
5. Run the focused sequence tests and existing submission regression. Review the complete diff independently. Document the subsystem audit and unimplemented overlap/range/eviction prerequisites. Publish separate mechanism commits and a Draft targeting master; preserve pr_build.yml unchanged.

Review focus: cycles must not form completion loops through semaphoreUnsignalCycle; do not publish layout/cycle changes on recording failure; retain resources before submission; preserve CpuDirty when a CPU write races staging; no assertion of complete sequencing or device graphics correctness.

Review correction: the new pre-submit failure paths exposed a cancelled-slot reuse bug. Test production FenceCycle cancellation state before fixing it: preserve inherited semaphore state for unsubmitted cancellation, verify fresh/pending/consumed signals and repeated cancellation. Add the real producer-copy-upload completion graph test. This is a separate prerequisite commit; no new wait is added.
