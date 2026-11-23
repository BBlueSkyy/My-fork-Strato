# Experimental TextureManager V3 on current master

PR #353, branch `wip/texman-v3`. Original head `35ae5b3d7ed0ecebdeb0fe19846ab18c025a0c68`; merged master `e8e38237aa5993deeaaae9f01f39ea08500f18db`. Both histories are retained with a merge commit. No merge into master, new feature work or additional runtime instrumentation.

## Architecture and overlap with master

| Mechanism | Current master | Refreshed V3 |
| --- | --- | --- |
| Ordered physical mapping identity / logical mip offsets | #385 validates ordered spans, including clipped ends | Retains cached `GuestResourceRanges`, including byte-equivalent repartitioning; all spans must match |
| Nested and secondary physical overlaps | #385 start-sorted mapping vector plus batched `MappingOverlapIndex` | Retains `TextureMappingCache` with all-segment indexing, unique `TextureStorage` owners and prefix-end searches; no duplicate manager index |
| Materialization over GPU-written overlaps | #385 discovers previously missed writers; #484 submits queued writers and reads them back safely | Uses #484's explicit pending-write receipt and ordered resource locks, with V3 guest-memory leases across the retry boundary |
| Backing / authority | Texture plus AliasGroup | Same transitional V3 division: TextureStorage owns immutable guest identity and Texture; Texture retains images, fences and AliasGroup authority |
| Access hazards | Legacy IntervalList dirty/sequenced sets | Retains V3 half-open/coalesced UsageTracker, independent submission and completed-wait lifetimes, RT and storage write hooks |
| Transfer failures / Free Guest | Recent master preserves alias writer state | Retains V3 transfer rollback for standalone and aliased images, separate lifetime attachments, guest padding/tail protection and implicit alias bridge restoration |

The #385 algorithms therefore overlap the V3 goals, but do not replace its storage/cache boundary or hazard model. The production manager deliberately continues using the V3 mapping cache rather than maintaining both caches. No new eviction or authority design is introduced.

## Integration decisions

- Preserve master sparse sampled reconstruction **before** ordinary physical-mapping validation; holes never enter the ordinary MappingCache. Preserve resident-span mirrors, remap traps, unstable-snapshot handling and lazy image layout initialization.
- Exclude replaced/retired aliases from V3 lookup and overlap readback. Keep #484 pendingGpuWriteTag semantics; do not infer queued commands from historical write serials. Retain V3 guest-memory leases while submitting pending overlap writers.
- Keep current master sampled-layout / tile-width spacing math, layer/backing/view semantics, native/converted-format policy, mip promotion and cached-view generations.
- Keep current master WaitSubmit queue ordering before inline synchronization, predecessor-cycle lifetime chaining, null sampled-image handling and pending-write receipt clearing.
- Preserve texel-buffer specialization, shifted handles, float/UINT/SINT identity and backing-aware view retention from #476. Buffer source changes relative to master only adapt UsageTracker calls and restore its direct IntervalList include.
- Shader compiler and all other submodule revisions are exactly those of master. No driver, shader backend, scheduler, GMMU, NCA or build-mode changes.
- Existing PR Release workflow and required master gates remain; V3 mapping/usage/authority gates are retained. APK remains ARM64 DevRelease, without validation layers or new instrumentation.

## Validation

Passed on the integrated tree:

- UsageTracker ASan/UBSan: boundaries, independent lifetimes and 180,000 randomized overlap queries.
- Guest ranges and MappingCache ASan/UBSan: ordered/repartitioned mappings, nested/later-segment overlaps, randomized full-overlap oracle and owning storage lifetime.
- Complete production FindOrCreate and texture authority methods on lavapipe with Vulkan synchronization validation and UBSan: pending producers with Free Guest OFF/ON, readback failure/retry, no-extra-copy paths, retained sources/leases, alias history/retirement, CPU traps, standalone transfer failures, read-only writer gates, view promotion, 1-to-8 mip expansion and BC views. Zero Vulkan validation errors.
- Production submission/FenceCycle UBSan: WaitSubmit independent of GPU completion, pending/cancel races, sparse sampled refresh, predecessor ordering and retained buffer lifetimes.
- Sampled-layout ASan/UBSan: width spacing/mip/array layout oracles and alias authority/invalidation/retry; sparse sampling mirror/self-wait checks.
- View-range UBSan; #385 mapping/overlap helpers ASan/UBSan; per-page TrapManager ASan/UBSan.
- Pitch layout/copy/decode ASan/UBSan: 310 round trips and 24 BC1/BC3 cases. Updated only stale host metadata declarations for the existing master backing-layer/spacing API.
- 1,186 TIC descriptor/specialization checks and backing/format/owner-aware texel-view lifetime checks.

The host fixtures compile the actual production methods; allocator/fence/Android memory transport boundaries remain explicitly represented by facades. The authority suite uses synchronized test Vulkan transport, so zero fixture errors are not a complete audit of every Android runtime barrier.

## Remaining boundaries

This is an **experimental device-test build**, intended for SpongeBob Cosmic Shake and Mario Kart 8 Deluxe. No gameplay, FPS, frametime or RAM improvement is claimed. The V3 Free Guest padding policy may retain more guest RAM. Cross-channel submission-wide alias serialization, general partial-region authority, cache eviction and new copy routes remain outside this update. Do not merge on host results alone.
