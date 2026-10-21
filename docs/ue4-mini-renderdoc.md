# UE4 graphics mini-RenderDoc: SpongeBob Cosmic Shake

This is a bounded GPU trace and raw-image capture adapted from PR #336.
It does **not** produce official RenderDoc `.rdc` files and **does not
fix** any graphics bug.

First target: SpongeBob SquarePants: The Cosmic Shake. The screenshots
show white clipping and green/purple/black corruption in menus, cutscenes
and gameplay. Geometry and UI remain at least partly recognizable.
This is not proof of a specific shader, driver or texture error.

## On-device reproduction

1. Install the **DevRelease** APK for this draft PR. Diagnostic branches
   `debug/ue4-*` deliberately do not build the broken Reldebug variant.
2. Load a scene where the colors are consistently wrong.
3. Open the Strato public application files at `gpu_capture/ue4`.
4. Rename `ARM_CAPTURE.rename_this_file_when_ready` to exactly
   `ARM_CAPTURE` (or create an empty file with this name).
5. Return to gameplay. When `capture_001/capture_complete.txt` appears,
   also wait for the **asynchronous** `MINIRD_UE4 wrote snapshot` log
   messages before copying the folder. The completion marker only means
   six presentations happened; pending pass readbacks can continue.
6. Send `capture_###.zip`, the matching `emulation.log`, and a
   screenshot or short video of the tested scene.

No game ID or shader hash is hard-coded. Repeat for other UE4 games.

## Files

- `present_source_00..05_*.raw`: six source textures sampled *before*
  the Android swapchain copy, when eligible.
- `present_00..05.txt`: width, height, Vulkan image format, layout,
  backing, source identity, crop and transform.
- `pass_XX*.raw` and `pass_XX.txt`: candidate full/half-resolution
  sampled color and postprocess outputs, read at real render-pass endings.
  These are linear staging bytes, **not PNGs**. Decode according to the
  filename and metadata Vulkan format (including RGBA16F for HDR).
- `gpu_events.txt`: Maxwell3D draw and shader hashes, texture bindings,
  render target formats, blend state, Fermi2D, MaxwellDMA and failed TIC
  mapping diagnostic events.

Budget: six approximate present intervals, at most 6 pass snapshots
per interval, 30 pass snapshots total, 160 MiB total pass-image bytes,
20 MiB per eligible image and 12000 events. No sampling, GPU waits or
transfer copies are performed unless capture has been manually armed.

## Investigation

1. Compare `present_source` with the actual screen. If already clipped,
   the corruption predates Android presentation.
2. Associate `pass_XX` snapshots by executor, render pass, submission
   and source/destination texture identity. `present_index_approx`
   is not a reliable guest-frame/draw ID due to asynchronous execution.
3. If the sampled scene color is correct but the output clips, inspect
   the responsible shader, output format, gamma/sRGB, blending and
   HDR-to-LDR postprocessing conversion.
4. If the sampled input is already bad, trace upstream, considering
   texture layout/format, stale views/aliases, synchronization and
   descriptor correctness. Evaluate unmapped TIC and DMA events
   independently rather than assuming causality.

Opt-in readback changes timing during capture. The first candidate pass
may not be the first incorrect pass, so a second targeted capture may
be required. Keep this PR in Draft; **never merge as a rendering fix**.

## Phase 3: focused HDR draw and tone-map capture

The first Cosmic Shake capture established that the pre-Android present source
already contains corruption and that the R11G11B10 HDR buffer contains invalid
float encodings. It did **not** prove the first offending draw or the cause.

Phase 3 replaces the 30 generic pass snapshots with GPU-ordered individual
draw probes. The folder `gpu_capture/ue4/ARM_CAPTURE` and six-present trigger
remain unchanged. The `focus_XX.txt` / `focus_XX_*.raw` files now include:

- `hdr_before_1`, `hdr_after_1` etc for HDR output on observed draw
  ordinals 1, 32, 128, 200 during the first two approximate intervals.
  A pre/post pair corresponds to a single executor-ordered draw.
- `tonemap_hdr_input_*` for the actual R11G11B10 source bound by fragment
  shader `9CDBA917AFDC0187`; `tonemap_aux_input_*` for its selected
  tiny 1x1/32x32 sampled resources; and `tonemap_bgra_output` immediately
  after that draw, once per approximate interval.
- `FOCUS_DESCRIPTOR` logs report **resolved** sampled texture/view,
  mip/layer and Vulkan format. `FOCUS_CBUF` reports the bound constant
  buffer slots and view sizes, but deliberately does not read GPU-dirty CBUF
  contents or assert that every listed slot is read by the shader.
- Separate global budget: max 48 focus images and 144 MiB. A skipped image
  logs FOCUS_SKIP or FOCUS_BUDGET_EXHAUSTED. Each image copy happens on
  the same GPU command stream, with a forced pass boundary only when armed.

These probes can affect frame pacing and pass coalescing **while capturing**.
Compare HDR before/after images *only within the same draw ordinal and
executor* to locate when new NaNs appear. The two approximate frame
intervals are not exact GPU frame boundaries, and the selected ordinals
may miss the first offending draw. If so, retarget ordinal selection using
these captures. A captured input being bad does not automatically implicate
the consuming shader.

Wait for all `MINIRD_UE4_FOCUS wrote snapshot` log lines before zipping.
Do not merge this diagnostic PR.
