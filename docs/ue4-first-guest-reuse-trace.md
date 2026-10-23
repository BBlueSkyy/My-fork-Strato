# First observed TexMan reuse after FreeGuest (#389 vs #390)

This instrumentation is **Android-only, diagnostic-only** and is added
identically to both Draft experiments, preserving their sole behavioral
difference:

- PR #389: logically evicted guest pages **retain old physical contents**
- PR #390: logically evicted guest pages are **zero-filled** (no MADV_REMOVE)

Both keep `GpuDirty` / `Clean` transitions, alias behavior, readbacks,
the original #388 `TEX_OWNER` tracing and the ARM64 Release build.

The extra bounded `TEX_REUSE` logs sample 8 windows of up to 64 bytes
across the existing guest mirror and report a sampled hash (not complete
contents), texture identity, backing guest mapping, format and dimensions,
dirty state, GPU write and readback serials:

- `immediately-after-free`: physical bytes after logical eviction
- `cpu-read-trap-before-readback` or
  `cpu-write-trap-before-readback`: first **observed CPU trap** on that
  texture after eviction, before any readback in its callback
- `overlap-before-reconciliation`: a new TexMan image description
  overlaps evicted guest data
- `new-texture-after-overlap`: the newly created texture's guest
  mirror after normal overlap reconciliation
- `texture-upload-before-guest-read`: a later host upload is about to
  consume that texture's guest mirror
- `synchronize-guest-before-readback`: readback is requested directly
- `after-successful-readback`: bytes after the next successful readback
- `readback-unavailable-after-reuse`: readback was skipped by the
  pre-existing layout/format check

**Important limits:** This records the *first TexMan-observed path*, not
necessarily the first physical access by the Switch CPU, DMA or Vulkan;
direct memory accesses outside these code paths are not intercepted.
Sampled hashes are not a proof of texture correctness. Limits prevent
unbounded logging, but traces can still alter timing and FPS.

## Device A/B

1. Use new APKs produced from the updated #389 and #390 commits, both
   **ARM64 DevRelease** (never RelDebug), same driver and scene.
2. In both cases set **Free Guest Texture Memory ON** and **Fast GPU
   Readback Hack OFF**.
3. Capture `emulation.log` from boot through the same SpongeBob corrupted
   scene, plus comparable screenshots.
4. Send both logs. Group by format, dimensions and guest region within
   each boot (ASLR means raw pointers differ across boots); compare
   `TEX_REUSE` hashes/paths and `TEX_OWNER` events.

The question to settle: is an evicted page reused as input to a texture,
or is it correctly reconstructed from GPU data first? This experiment
does **not** modify the answer or propose a permanent fix.

Do not merge into master. Do not benchmark FPS on instrumented builds.
No new mandatory tests, no workflow modifications, no Actions monitoring.
