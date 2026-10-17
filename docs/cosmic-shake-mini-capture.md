# Cosmic Shake: bounded mini-renderdoc (diagnostic only)

The screenshot corruption in *SpongeBob SquarePants: The Cosmic Shake* is not proof of
a TextureManager, shader, or driver defect. This branch observes selected GPU pass
inputs and outputs without substituting textures, changing shaders or altering
texture matching.

## Trigger and export (Android 12+)

1. Install the **Dev** APK from this Draft PR.
2. On the Strato Dev main screen **long-press the log icon** (normal
   tap still shares the emulator log). Select **Armar captura para o
   próximo jogo**. This creates `gpu_capture/cosmic_shake/ARM_CAPTURE`
   directly in app storage: no Android/data access required.
3. Open Battle for Bikini Bottom Rehydrated or Cosmic Shake. When it
   reaches presentation, the native code consumes the trigger and
   captures up to six presented frames. The capture does not require
   reaching the main menu, but a crash before presentation cannot
   generate GPU pixel readbacks.
4. Return to the Strato Dev main screen; long-press the log icon again,
   then choose **Compartilhar captura/diagnóstico (ZIP)**. The Android
   Sharesheet offers a ZIP from the app's own DocumentsProvider.
5. Send the ZIP and driver name/version. Even if no capture triggered,
   `diagnostic-status.txt` and `emulation-tail.txt` should help
   determine whether the code reached presentation.

The manual trigger file may alternatively be renamed using Android's
Files picker through Strato Dev's document provider. The automatic
blackout trigger is not reliable for continuously corrupted scenes
and should be left unarmed. This is diagnostic-only; do not merge.

## Output

- `texman_decisions.txt`: texture reuse/matching, guest mappings, mips/layers.
- `tic_gmmu_faults.txt`: up to 256 distinct unmapped TIC IOVAs with
  TIC index, shader type, current submission/render-pass counter,
  requested texture size, mapped/unmapped segment ranges, first invalid
  offset, dimensions/mip count, format, tiling and a summary of lookup
  occurrences during six captured frames. This associates descriptor
  lookup context with nearby GPU work, **not** a proven shader draw ID.
  The original null-texture fallback is unchanged.
- `pass_boundary.txt` / `pass_NNN.txt`: candidate output images, pass
  sequence, image views, formats, host buffer statistics/hashes and raw preview.
- `pre_pass_NNN.txt`: paired input state before affected 1280×720 passes,
  where format and transfer preconditions allow.
- `focus_NNN.txt`: nine 4×4 sampled-input probes for supported
  RGBA/BGRA or raw compressed BC7 blocks, with hashes and basic statistics.
- `pre_pass_NNN.ppm` / `post_pass_NNN.ppm`: downsampled 160×90
  visual previews **only** when the captured output is uncompressed 8-bit
  RGBA/BGRA; not a complete Vulkan or RenderDoc frame capture.
- `pretrigger_*.txt` when monitoring was active.

The normal RenderDoc API remains separate and requires an actual injected
RenderDoc Vulkan layer; this diagnostic is a bounded, built-in substitute.

## Limits and cautions

Six presented frames, 36 full pass snapshots, 96 MiB of full-stage buffers,
64 focused probes and 128 KiB of probes, plus bounded metadata. The full
readback filter targets 1280×720 scene compositions and 1920×1080 outputs;
other resolutions may only produce metadata. Previews are based on linear
staging bytes (not a decoded/colour-managed scene). A 1280×720 pass can be
sampled before and after its Vulkan render pass but not after each draw.
Extra transfer operations and barriers may alter synchronization or timing;
compare behavior with the capture unarmed. Nothing here proves causality on
its own. **Never merge this diagnostic branch into master.**
