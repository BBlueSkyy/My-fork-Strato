# Cold regional lookup retry: startup regression investigation

## Proven regression and limit of the evidence

`baaf3cd2` is the device-reported last revision that reached SpongeBob: The Cosmic Shake's menu. The next commits are `45452491` (regional authority) and `ecdfb347` (resource-first publication). A deterministic production-code test isolates a **non-progress retry introduced in `45452491` and still present in `ecdfb347`**. It does not establish that this is the exact point where the game stopped on the device.

The already supplied `emulation(20261026-130452).log` identifies Cosmic Shake v1.0.0 and Turnip Adreno 650, and ends after initial GPFIFO shader-compiler warnings. It contains no regional entry/exit traces or thread stacks that identify registration, drain, reconciliation or a deadlock. Its final shader warnings do not prove a shader cause; no shader change was made.

## Isolated mechanism

The normal descriptor preparation loop resolves and attaches one texture before resolving the next (`GetTextureBinding` / `GetImageBinding`). Reproduce that order inside `RunWithTextureBackingRetry`:

1. Attach existing A, keeping its executor-context resource lock.
2. Find a previously unseen partially overlapping B.
3. `RegisterRegionAuthority` expands the component and checks for any member already owned by this context. That safety check throws before acquiring cross-resource locks. It precedes compatibility rejection, so an incompatible cold lookup can trigger it too. The early `DrainRegionAuthority` path for a regional image changing to an exact alias has the same context check.
4. The old handler only called `Submit`, read back `overlapReadbacks` and replayed preparation. It neither completed the registration/drain nor cached B.
5. Replay attaches A again, and step 3 repeats indefinitely. Repeated submission alone cannot make this operation progress. This is a **retry loop**, independently reproduced without concurrent threads or GPU fence blocking, not a demonstrated `ReconcileRegions` deadlock.

The test-only oracle stops after the fourth attempt so a failure cannot hang the test process. Production has no retry cap, timeout or forced synchronization. Replacing just `TextureManager` with its actual `baaf3cd2` body makes the cold-lookup oracle succeed. Replacing it with `45452491` or `ecdfb347` reproduces the loop, holding the current safe publication protocol constant. This is a controlled source comparison, not an APK/game bisection.

## Small correction

Only regional registration/drain calls inside `FindOrCreate` decorate their existing control-flow request with the owning manager and a value copy of the requested guest layout. After preparation unwinds and `Submit` releases executor resources, the handler finishes that lookup with no executor tag. It publishes the cache/domain transition **before** replay can reattach A. The replay reuses the prepared image/view.

The manager is GPU-owned and the synchronous handler remains within the channel lock. The request retains the affected texture/component references and copies physical mapping spans, not interrupted descriptor/allocator pointers. Lookup allocation failures propagate; no successful cache entry or fabricated ownership is reported. The next invocation can retry normally.

The regional context checks, CPU-callback guards, donor revalidation, version checks and submission dependencies remain intact. `RegisterRegionAuthority`, `ReconcileRegions` and `DrainRegionAuthority` themselves are unchanged. Exact alias registration requests outside these regional calls keep their original handler. No new sequencing subsystem, draw-time scan, global wait or production instrumentation is added.

## Validation

Commands run against real Vulkan/lavapipe image contents with synchronization validation and UBSan:

- `TEXMAN_COLD_LOOKUP_PROBE=1 python3 tests/gpu_backing_promotion/run.py`: compatible partial lookup, incompatible dimensions, misaligned partial fallback/drain, and exact-alias drain after attaching A. All finish in two attempts with FreeGuest OFF/ON and format-list OFF/ON. Red/green/exclusive bytes and retained A view are checked. Repeating cached lookups and adding an independent texture requires no further submissions.
- Deferred allocation failure preserves both old component references, backings/views and latest writers. A subsequent retry drains successfully and preserves red exclusive / green shared bytes. This recovery case is also checked with FreeGuest OFF/ON.
- Negative controls use `TEXMAN_BASELINE_MANAGER_REF=baaf3cd2`, `45452491` or `ecdfb347` with `TEXMAN_COLD_LOOKUP_PROBE=1`. The baseline-header fixture also needs `CXXFLAGS="$CXXFLAGS -I$PWD/app/src/main/cpp/skyline/gpu/texture"`. Historical deferred-lookup failure tests are inapplicable; the progress/content oracle remains active.
- `TEXMAN_AUTHORITY_PROBE=1 python3 tests/gpu_backing_promotion/run.py`: **0/1024 stale shared texels**, exclusive bytes, reverse writers, CPU callbacks, changed donors, descriptor/view retention, failures and FreeGuest OFF/ON. Includes the new cold-lookup cases. ASan/UBSan pass as well.
- `TEXMAN_LOCK_ORDER_PROBE=1 python3 tests/gpu_backing_promotion/run.py`: both format-list modes pass.
- Full backing-promotion suite plus texture-sequence, submission, sampled-layout, storage-preflight, overlap-materialization, draw-texture and framebuffer-cache regressions pass. No Vulkan validation errors.
- `git diff --check` passes. APK workflow/build configuration unchanged: ARM64 DevRelease (Release), no RelDebug or additional required CI tests. No merge or Actions monitoring.

The new progress tests use actual production lookup, region, attachment and retry methods with real resource locks/images. Executor submission/graph boundaries remain host facades: the new cold-lookup writers have already reached Vulkan submission. Pending queued-producer submission is covered separately by the existing production pending-materialization and submission tests. No complete Android build or device launch/FPS measurement was performed here.

## Remaining uncertainty

A Release device run must establish whether this narrow correction restores SpongeBob's menu. Without such a run or a stuck-thread trace, it is not possible to assert that the demonstrated loop was the game's actual stopping point, or to exclude another wait/retry in its workload. No UE4 graphics or FighterZ performance correction is claimed. If startup still stops, the next evidence needed is the current GPFIFO stack and whether it is repeating the regional preflight or blocked in a specific resource/fence wait; broad shader/capture investigation is unnecessary for this question.
