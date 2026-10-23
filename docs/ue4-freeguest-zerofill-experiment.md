# Free Guest Texture Memory: zero-fill versus MADV_REMOVE

**Diagnostic experiment only. DO NOT MERGE.**

This is a direct follow-up to #388 (ownership instrumentation) and #389
(keep pages physically resident, mark them logically evicted).
In the same SpongeBob: The Cosmic Shake scene, #388 with Free Guest ON
partially improved rendering, while #389 with ON brought back the
lighting/texture corruption.

On Android, for textures eligible for `FreeGuest()`, this branch:

- Keeps the original `CpuDirty` / `GpuDirty` transitions, logical
  `memoryFreed=true` flag, GPU readbacks, aliases and `TEX_OWNER` tracing.
- Calls a separate diagnostic memory-manager method, which translates the
  mirror mapping, makes **the same interior whole-page range** writable as
  `FreeMemory()`, and fills it with zeros instead of `MADV_REMOVE`.
- Leaves existing `FreeMemory()` untouched for all other callers;
  non-Android keeps the original `FreeMemory()`.
- Logs `TEX_OWNER event=zero-filled-guest-pages`.

Only page-aligned interior bytes are zeroed, exactly as in
`MemoryManager::FreeMemory()`; partial boundary pages are left intact.

## One test

Use this PR's Android ARM64 **Release** APK, **Free Guest Texture Memory
ON**, **Fast GPU Readback Hack OFF**, the same Turnip driver and the same
SpongeBob scene as #388 and #389. Save a screenshot plus `emulation.log`.

- Improvement matching #388: overwriting stale page contents with zeros
  may explain at least part of the benefit; this does not prove a valid
  permanent fix.
- Corruption matching #389: actual `MADV_REMOVE` behavior or some effect
  beyond page contents matters.
- Partial/intermittent outcome: inspect per-texture `TEX_OWNER` events
  and avoid inferring a general cause from one boot.

This operation performs potentially large CPU writes and **must not be
used for performance benchmarking** or shipped as a permanent path.

Reuse the existing PR APK `assembleDevRelease` (ARM64 Release)
workflow. No RelDebug, new mandatory tests, or Actions monitoring.
