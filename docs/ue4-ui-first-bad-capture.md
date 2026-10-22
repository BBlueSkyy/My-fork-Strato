# UE4 UI mini-RenderDoc — first corrupted surface

Opt-in diagnostic branch for the repeated horizontal-band displacement, left-side noisy rectangles and UI misalignment observed in Demon Slayer: The Hinokami Chronicles and other UE4 games.

## Android steps

1. Install **ARM64 DevRelease** APK from this Draft PR, not DevReldebug.
2. Open the affected menu, keep it on screen.
3. In Strato's public app files, open `gpu_capture/ue4_ui`.
4. Rename `ARM_CAPTURE.rename_this_file_when_ready` to `ARM_CAPTURE`.
5. Return to the emulator for at least four presentations; polling occurs about once per 16 presentations.
6. Wait for `pass_readbacks_done.txt` **inside capture_###** before copying the folder. It appears only after all reserved asynchronous pass readback callbacks finish. Check `requested`, `written` and `failed`. If the marker never appears, keep the `emulation.log` to diagnose stalled callbacks. `capture_complete.txt` only marks the four source-frame samples.
7. Zip the generated `capture_###` directory and send that zip with the matching `emulation.log` and a screenshot.

**This is not the official RenderDoc format** and it does not automatically create a .zip archive.

## Output and investigation

- `present_XX.txt`, `present_source_XX*.raw`: original VI/swapchain source color surfaces before Android presentation.
- `pass_XX.txt`, `pass_XX*.raw`: eligible full-sized color render targets sampled with GPU-ordered copies after actual pass endings. Includes executor, submission and render-pass identity.
- `pass_readbacks_done.txt`: final async readback completion and successful/failed write counts; the capture is incomplete without this marker when pass images were requested.
- `gpu_events.txt`: bounded RT_DIM (original vs surfaceClip-adjusted dimension/layer size), VIEWPORT, SCISSOR, PASS_TARGET, DRAW shader hashes, FERMI_BLIT, PRESENT_SOURCE, and pass snapshots.

Compare successive raw images using their recorded Vulkan format and dimensions (not all formats are RGBA8). If the first corrupted pass is identified, inspect its precise sampled inputs, CPU guest mapping and shader separately; do **not** clamp, shift, crop or mutate texture/layout behavior speculatively.

## Runtime bounds and limits

Opt-in only, four approximate present intervals, 5 pass images per interval, 20 maximum, 96 MiB total pass-image bytes, 12 MiB per pass, 10,000 logged events. Source images independently capped to 12 MiB per frame. One cold marker-file poll per 16 presentations. All normal GPU code paths are unchanged except an inactive-branch check. While armed, image copies and fence waits alter timing; **FPS under capture is not a benchmark**.

`present_index_approx` is NOT an exact guest GPU frame boundary. No promise of a perfect first bad draw; follow-up capture retargeting may be required. Shader SPIR-V binaries are not dumped in this initial small scope, only their hashes. No automatic ASan/CI requirements, no new pipeline/shader compiler/TexMan functional changes. Keep Draft, do not merge, do not monitor Actions.
