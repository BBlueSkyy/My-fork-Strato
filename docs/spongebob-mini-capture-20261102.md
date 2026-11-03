# SpongeBob: The Cosmic Shake — GPU Mini capture (diagnostic #421)

## Purpose

The PR #421 overlap logs for two runs reported `no_readback=0` in the sampled trace, 256 readbacks observed on each boot, and 128+ regional rejections. Both runs still showed comparable visual corruption. This does not prove wrong GPU textures; `VK_ERROR_OUT_OF_POOL_MEMORY` was also logged and may be handled by the descriptor allocator's resizing path.

This continuation captures **small regions of actual host VkImages** before the first eligible draw after a manual arm, plus the color target before the draw and after the encompassing render pass. It does not invoke guest readback, change Free Guest Texture Memory policy, replace shaders, force synchronization outside explicit capture, or modify the #400 branch.

The exporter format `SBMINI1` is a **custom raw GPU Mini format**, not a native RenderDoc `.rdc` capture.

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
- **Role 2:** color target **after the containing render pass finishes**. It is *not* guaranteed to be an after snapshot of only one draw, because later draws can belong to that same render pass.
- Metadata includes the VkImage identity (valid only within this process), backing generation, GPU write serial at queue time, native/guest format, mip, layer and ROI dimensions. Different processes must not be matched by pointer addresses.
- Each image copy is explicitly ordered within the existing executor command buffer, with image and buffer barriers. The exporter waits for the associated fence **on the export worker only**. A deliberately armed capture may alter pass boundaries or frame timing. Ordinary non-capturing emulation is unchanged except for one atomic armed check per AddSubpass.
- No guest buffer, uniform contents, shader binary or descriptor-set snapshot is included. Images with unsupported layout/view/aspect or more than 64 KiB for their ROI are skipped. A successful capture proves only the included inputs/target were sampled; it cannot exclude other faulty resources.

## Decode

```bash
python3 tools/decode_spongebob_mini.py spongebob-mini.mcap decoded/
```

Output: `manifest.json`, `op*-role*.bin` raw bytes for all records and PNG previews for RGBA8/BGRA8 VkFormats. The JSON includes per-operation count of changed bytes and mean byte difference between the target before and after. Nonzero bytes or a changed target do **not** by themselves prove correct colors. For compressed/HDR formats, the raw blobs are preserved for format-aware inspection.

After two captures, compare **format, dimensions, mip, role and same scene**; compare raw fingerprints only for matching resources. The first eligible operation may not draw a corrupted object. If no useful target is captured, instrument the relevant shader/renderpass ID instead of expanding capture indiscriminately.

## Safeguards and validation

- Only an explicit UI action arms a capture. Max 3 operations × 6 snapshots; max 64 KiB payload per image. No per-frame image dump.
- Retained staging resources/fences are released before the GPU allocator/device. A cancelled or unrecorded cycle cannot yield valid exported payloads.
- The exporter uses the Android SAF write-only fd directly with short/EINTR handling; no procfs fd reopen. The OS and GPU lifetimes are retained through export.
- `bash tests/spongebob_mini/run.sh` tests the capture controller and the file decoder (C++20/UBSan, Python 3). These are optional host tests, not new mandatory CI gates.
- Android compile, real Vulkan barrier/copy validation and device image interpretation remain to be confirmed by the APK/device. Do not merge into #400 or diagnose a root cause from these captures alone.
