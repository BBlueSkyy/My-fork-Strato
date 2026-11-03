# SpongeBob: The Cosmic Shake — GPU Mini capture (diagnostic #421)

## Purpose

The PR #421 overlap logs for two runs reported `no_readback=0` in the sampled trace, 256 readbacks observed on each boot, and 128+ regional rejections. Both runs still showed comparable visual corruption. This does not prove wrong GPU textures; `VK_ERROR_OUT_OF_POOL_MEMORY` was also logged and may be handled by the descriptor allocator's resizing path.

This continuation captures **small regions of actual host VkImages** before an eligible **Maxwell 3D draw** after a manual arm, plus the color target before that draw and immediately after it. The manually armed capture ends the render pass after the selected draw, so no later draw belongs to the sampled target delta. The manual split may perturb timing and pass compatibility; it does not run while disarmed. It does not invoke guest readback, change Free Guest Texture Memory policy, replace shaders, force synchronization outside explicit capture, or modify the #400 branch.

The new exporter format `SBMINI2` is a **custom raw GPU Mini format**, not a native RenderDoc `.rdc` capture. The decoder remains compatible with the previous `SBMINI1` format.

## Controls (Moto G100)

1. Install the ARM64 DevRelease APK from Draft #421 **when CI has succeeded**. Do not assume a build is valid until GitHub Actions has produced its APK.
2. Keep Turnip, game update and other settings identical to the previous SpongeBob test. Start with **Free Guest Texture Memory OFF**.
3. Enter the visibly corrupted title scene and keep it running, without pausing.
4. **Long-press the virtual-controller visibility icon**, or long-press the FPS display if the performance overlay is enabled.
5. Choose **Capture next graphics operation**. Leave the scene running so the next eligible color-target operation can be captured and submitted.
6. Optionally repeat the arm action up to **three total operations per process**, allowing at least a couple of seconds between arms. Do not arm multiple times rapidly; one arm captures one operation.
7. Choose **Export GPU Mini capture** from the same menu. Save `spongebob-mini-*.mcap` through Android's document picker while the game stays running.
8. Save `emulation.log` and a screenshot/video of the title screen. Note whether corruption is predominantly white/pink/black or different.
9. Close the emulator completely and repeat for the next boot. **Both captures are valid even when both boots are corrupt**.

If the UI says capture incomplete, the current process may not have reached an eligible operation or the render pass may not have ended; try again while emulation is running. A zero-byte file is not a successful capture. If the capture menu does not appear, use the controller visibility icon or enable the FPS overlay.

## What is saved

- A manually armed operation whose main color target is at least 320×240, is native color 2D, single sample, supports transfer source and has General layout.
- **Role 1:** color target 64×64 center region **before** the selected operation.
- **Roles 3–6:** up to four eligible sampled-input images, 64×64 region at top left (or smaller for small inputs), before the draw. Compressed formats remain raw block bytes.
- **Role 2:** same color target **immediately after the isolated selected draw**, after explicitly closing that diagnostic render pass. Unlike SBMINI1, a subsequent draw cannot be included in this before/after delta. A conditional draw may execute no fragments; an unchanged image alone does not prove a shader issue.
- Metadata includes the VkImage identity (valid only within this process), backing generation, GPU write serial at queue time, native/guest format, mip, layer and ROI dimensions. SBMINI2 additionally embeds the **pipeline state hash, six guest shader hashes, draw kind/topology/parameters, channel sequence, and sampled-input ordinal with cached TIC index and IOVA**. A TIC index of `4294967295` means unknown, and `matching_tics > 1` means more than one TIC cache entry resolves to that view; neither should be treated as unique provenance. Different processes must not be matched by pointer addresses.
- Each image copy is explicitly ordered within the existing executor command buffer, with image and buffer barriers. Only a manually armed Maxwell draw can be selected; clear/DrawTexture helper operations are not mistakenly labeled as shader draws. The exporter first **non-blockingly checks** whether the associated fence has finished; it waits only on an already-signalled fence from the Android export worker. If any snapshot is still pending, export fails without writing a partial payload and can be retried while the game runs. A deliberately armed capture may alter pass boundaries or frame timing. Ordinary non-capturing emulation is unchanged except for cheap armed checks and metadata construction only while manually armed.
- No guest buffer, uniform contents, shader binary or descriptor-set snapshot is included. Images with unsupported layout/view/aspect or more than 64 KiB for their ROI are skipped. A successful capture proves only the included inputs/target were sampled; it cannot exclude other faulty resources.

## Decode

```bash
python3 tools/decode_spongebob_mini.py spongebob-mini.mcap decoded/
```

Output: `manifest.json`, `op*-role*.bin` raw bytes for all records and PNG previews for RGBA8/BGRA8 VkFormats. The JSON includes per-operation changed bytes and mean difference, the six shader hashes (SBMINI2), and **bit-accurate NaN/+Inf counts for VkFormat 122 (R11G11B10)**. `target_new_nan_pixels` counts pixels that changed from finite to containing a NaN within the isolated draw. Nonzero bytes or a changed target do **not** by themselves prove correct colors. For compressed/HDR formats, the raw blobs are preserved for format-aware inspection.

After two captures, first compare **shader hashes, pipeline hash, draw parameters and matching target geometry**. **Never interpret two unmatched draw signatures as evidence of different texture contents.** Only when the draw signatures match compare format, mip, TIC/IOVA, bytes and target NaNs. The first eligible draw may not draw the corrupted object. If no useful matching draw is captured, select a specific shader hash for a later targeted test instead of expanding capture indiscriminately.

## Safeguards and validation

- Only an explicit UI action arms a capture. Max 3 operations × 6 snapshots; max 64 KiB payload per image. No per-frame image dump.
- Retained staging resources/fences are released before the GPU allocator/device. A cancelled or unrecorded cycle cannot yield valid exported payloads.
- The exporter uses the Android SAF write-only fd directly with short/EINTR handling; no procfs fd reopen. The OS and GPU lifetimes are retained through export.
- `bash tests/spongebob_mini/run.sh` tests the capture controller and the file decoder (C++20/UBSan, Python 3). These are optional host tests, not new mandatory CI gates.
- Android compile, real Vulkan barrier/copy validation and device image interpretation remain to be confirmed by the APK/device. Do not merge into #400 or diagnose a root cause from these captures alone.

## Draw-identification follow-up (SBMINI2)

The first two SBMINI1 captures both contained NaNs in a 1280×720 R11G11B10 target, but also had different BC input formats and image identities. Without shader/draw identity, those differences could be unrelated operations. This diagnostic version makes the comparison falsifiable: capture the same corrupt menu scene in two fresh boots, export `.mcap` files, and compare `pipeline_hash`, `shader_hashes`, `draw_kind`, `topology`, `count` and `instances`. Matching signatures do not prove identical buffers or textures, but are needed before interpreting different image bytes. An absent match is **inconclusive**, not a TexMan bug.

To pinpoint which draw introduces a NaN, the manual capture records the target immediately before and immediately after the selected draw. Draws after that point are excluded from the measured delta; earlier work may still affect the "before" input. The JSON field `target_new_nan_pixels` is derived by inspecting the exact NaN encodings, not by interpreting false-color previews. If NaNs are introduced, the responsible *draw* is isolated, but determining whether the shader, sampler, buffer, or texture is responsible still requires further evidence.

The caching of TIC metadata is observational only: no forced descriptor rebinding or texture-pool flush. Guest IOVA is not a host pointer. Distinct TICs may resolve to the same view and cached metadata can be ambiguous; this is represented explicitly.
