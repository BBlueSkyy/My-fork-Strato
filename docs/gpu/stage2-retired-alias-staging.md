# Stage 2A: reclaim download staging owned by permanently retired texture aliases

## Scope
This stage deliberately targets one existing, proven ownership boundary rather
than adding a new staging pool, eviction timer or per-frame collector. When
`Texture::RegisterAliases` successfully reconciles authoritative guest bytes,
representations classified as `historical` become `aliasStale`,
`aliasRetired`, and `replaced`. Such representations cannot perform a future
readback. A retained `downloadStagingBuffer` is then unused capacity until the
texture wrapper is destroyed.

Release only their cached `downloadStagingBuffer` **after** successful alias
registration, while their texture locks are already held. Prior
`Texture::SynchronizeGuest` readback explicitly blocks on its completion fence
before accessing staging on the CPU, so the cache can be dropped at this
retirement boundary. Existing shared ownership, if any, continues to protect
the actual staging resource. No generic texture eviction is attempted.

## Deliberate exclusions
- All usable/non-retired textures keep their staging buffers to avoid
  repetitive allocation and frame-time regressions.
- Upload staging remains bound to existing `FenceCycle` dependencies.
- No texture image, image view, shader cache, renderpass, buffer mapping,
  descriptor, GPU authority, or guest-memory policy is changed.
- No Vulkan fence query, GPU idle wait, worker, general pool, frame loop or
  global memory budget is introduced.
- This patch is independent of GC PR #441; the two can be reviewed separately.

## Regression checks
- `python3 tests/gpu_sampled_layout/run.py`: production alias-registration
  extraction checks stale staging ownership expires while the surviving alias
  keeps its staging allocation. This suite is in the normal PR build gate.
- `python3 tests/gpu_backing_promotion/run.py` on a Vulkan-capable host:
  checks that failed/readback-blocked registration retains historical staging,
  and successful registration releases it. Existing backing coherence and
  frame-buffer tests provide the surrounding coverage.
- CI Android DevRelease and real-device FPS/frametime/driver tests are still
  required. Do not infer a general memory gain from this narrow reclamation.

## Risks
`vkDestroyBuffer`/VMA free may execute at the retirement site when the staging
has no other owners; the retirement path is cold, but actual driver cost is not
measured. Under real games, impact only exists when a historical texture alias
holds a cached download staging allocation. No change is expected for games
that never enter this path.
