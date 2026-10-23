# UE4 Guest Texture Ownership A/B Diagnostic (Release APK)

This is intentionally a **diagnostic-only Draft PR**, not a texture
management fix. It uses the existing ARM64 `assembleDevRelease` APK
workflow; no RelDebug, validation layer, additional CI, or gameplay
changes.

## Evidence prompting this diagnostic

SpongeBob: The Cosmic Shake shows partial graphical improvement with
**Free Guest Texture Memory** enabled, while ordinary emulation logs
do not expose the first relevant `DirtyState` transition.

`CommandExecutor::SubmitInternal()` calls
`SynchronizeHostInline(..., true)` on *all* attached textures,
including sampled-only resources. The existing Free Guest Texture
Memory setting changes how this hint is interpreted for textures
that have never been written by the GPU.

## Instrumentation

Android-only `TEX_OWNER` logs (first 10 transitions per texture,
6000 total; two sampled 512-byte fingerprints per texture, 4000 total)
for textures >=128x128:

- policy decision on first host/inline attachment for sampled-only
  texture (`requestedGpuDirty` and `actualGpuWrite` flags)
- transition during host upload, and Clean -> GpuDirty paths
- actual GPU writer registration / serial
- FreeGuest memory discard versus first guarded attempt
- CPU write traps and GPU-to-guest readback start/completion/unsupported
- sampled fingerprints of guest bytes **before upload** and staging
  bytes **after deswizzle** (not full dumps; hashes do not establish
  semantic pixel correctness)

Each event includes the texture and guest mapping pointers, dimensions,
format, dirty state, guest-memory-freed flag, alias/read-only flags,
serials and pins. Memory addresses can differ across boots.

## Capture procedure

1. Use the same APK, shader cache setup, game, driver and scene.
2. Keep Fast GPU Readback Hack **OFF** and do not change other options.
3. With Free Guest Texture Memory **OFF**, restart the game and capture
   `emulation.log` from boot until the broken textures appear.
4. Fully stop the game, enable Free Guest Texture Memory, restart in
   the **same scene**, and capture the second log.
5. Send both logs, and ideally screenshots from the same camera angle.
6. Compare `TEX_OWNER` by format/dimensions/mips/guest mapping
   neighborhood, then inspect the first difference in
   `host-attachment-policy`, `inline-clean-to-gpu`,
   `free-guest-pages` and `guest-readback-complete`.

These events are bounded and focus on ownership/copy transitions, but
they **do not prove the GPU image contents** at the failing draw.
If the first difference remains ambiguous, a targeted texture ID
capture (actual guest + staging + Vulkan readback) is needed.

Do **not merge** this diagnostic PR into master.
