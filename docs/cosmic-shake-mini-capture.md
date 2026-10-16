# Cosmic Shake: bounded mini-renderdoc (diagnostic only)

The screenshot corruption in *SpongeBob SquarePants: The Cosmic Shake* is not proof of
a TextureManager, shader, or driver defect. This branch observes selected GPU pass
inputs and outputs without substituting textures, changing shaders or altering
texture matching.

## Trigger

1. Install an APK from this draft branch and enter the corrupted main menu.
2. Locate the app's public files directory, then `gpu_capture/cosmic_shake/`.
   If no directory appears, present several frames first.
3. Rename `ARM_CAPTURE.rename_this_file_when_ready` to `ARM_CAPTURE`.
4. Let six frames present, then exit the game and preserve
   `gpu_capture/cosmic_shake/capture_001/` (or the next numbered directory).
   On Android with restricted app-directory access, retrieve the folder with
   an authorized file manager or ADB where permitted.
5. Send the capture folder (ZIP), the emulator log, driver name/version and
   a screenshot of the corrupted frame.

Manual trigger is preferred: the older automatic blackout detector is not a
reliable signal for a scene that is corrupted continuously. Leave AUTO_CAPTURE
unarmed. No capture occurs unless explicitly armed.

## Output

- `texman_decisions.txt`: texture reuse/matching, guest mappings, mips/layers.
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
