# TextureManager v3, stage 1

Base: master 66637da0. Branch: wip/texman-v3. Draft #353; #244 unchanged.

- UsageTracker: private, coalesced half-open physical ranges; execution writes
  and uncompleted GPU hazards have separate lifetimes. GPU writes invalidate
  sequenced caches, and only a completed executor wait retires GPU hazards.
- Upload failure leaves guest bytes authoritative and retryable. Resource/fence
  retention precedes submission for standalone images as well as aliases.
- Readback publishes dirty state, freed-memory status and readback serial only
  after actual encoding succeeds. Existing converted-format fallback remains.
- Stale aliases must reconcile before claiming a GPU write. Restore discarded
  current-writer bytes before validating an implicit alias reference.
- Free Guest Texture Memory preserves padding/tails that a different layout can
  expose as texels. Only byte-complete native 2D block layouts may discard pages.
  This trades retained guest RAM for preserving otherwise unrecoverable data;
  device memory use and frame time still need testing.

Validation: `python3 tests/gpu_usage/run.py` (ASan/UBSan, 180000 randomized
queries); `python3 tests/gpu_backing_promotion/run.py` with Clang, lavapipe,
validation and UBSan; existing queries/conditional/draw-texture suites.
Transfer tests inject allocation, attachment and recording failures, retry and
compare bytes. Existing alias, trap, mip growth and view/promotion tests run too.

The Vulkan harness was stale on master: missing pitch helpers/GetSize, fake
DirtyState incompatible with extracted methods, ambiguous Extent3D assignments,
outdated rejection of valid read-only aliases, and unretired test-only command
buffers. Its adapters now match production; authority decisions remain extracted
production methods. Vendor headers are system includes; Clang accepts production
Vulkan initializer ordering with the same style warning disabled as on Android.

Reference comparison: Ryujinx TextureGroupHandle at 2382717600939e0e970f47f404cd62672c097ad4
keeps modified state until synchronization is ready and distinguishes deferred
copies from authoritative writes. Strato retains its own fences and guest-memory
bridge; no dependency-graph algorithm was imported.
https://git.axenov.dev/Museum/ryujinx/src/commit/2382717600939e0e970f47f404cd62672c097ad4/Ryujinx.Graphics.Gpu/Image/TextureGroupHandle.cs
