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
  ordinals 1, 32, 128, 200 during the first two intervals in which HDR draws actually occur.
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

### Phase 3 follow-up: missed HDR draws fixed

The 2026-10-21 capture contained 20 successful converter snapshots (4 HDR
inputs, 4 BGRA8 outputs and 12 1x1 auxiliary inputs), but **zero**
`hdr_before_`/`hdr_after_` images. The draw selector incorrectly only
accepted present indices 0 and 1, while the HDR draws began in interval
2. The corrected selector starts on the first observed HDR draw and
allows the following interval, regardless of the initial present index.
Its state resets on each new manual session and maintains all snapshot
quotas. No GPU code is modified unless capture is explicitly armed.

The HDR values (median R/G/B approximately 2032/1184/864) already reach
the converter input. About 0.294% of pixels had a NaN component, but
69.8% of pixels in the converter's BGRA8 output were almost white.
These measurements implicate upstream HDR values, without proving the
first responsible shader or the origin of their amplification.


## Phase 4: shader-group transition probes (draws 2–31)

The latest HDR before/after pairs show 0% NaNs before draw 1,
five pixels after draw 1, and ~20.15% before draw 32.
The direct draws between these points use the following fragment
shader groups (based on the previous captured trace):

| Draw ordinals | Fragment shader |
| --- | --- |
| 2–18 | `7161B14045F710B5` |
| 19–22 | `EA18B70958415FF3` |
| 23–31 | `9784BD03FBB0AAC9` |

The phase-4 manual capture now records HDR before/after pairs around
ordinals **2, 18, 19, 22, 23 and 31**, in each of the first two intervals
that contain HDR draw commands. All other draw ordinals incur only
the active-session counter check; there are no new copies.

For each selected draw, `gpu_events.txt` now records:

- `HDR_DRAW_FOCUS`: draw ordinal, executor, submission, render dimensions,
  shader hashes, index/instance counts, and output view format;
- `HDR_DRAW_IMAGE`: up to 24 *resolved sampled image views*, with actual
  image dimensions, view format, backing format, mip/layer range and layout;
- `HDR_DRAW_CBUF`: CBUF slots referenced by uniform descriptors,
  their bound sizes, and the compiler's declared used byte counts;
- `HDR_DRAW_TEXTURE_DESC`: shader-declared sampled image descriptor
  CBUF index/offset/type/count (not a dump of private texture content).

These are diagnostic metadata only. The capture does **not** read
GPU-dirty CBUF bytes, modify shader code, or change descriptors.
It may still split render passes and lower FPS **while armed**.
The existing 48-image / 144-MiB budget is retained.

The selected group boundaries may locate the responsible group, but
are not guaranteed to find the *first* defective draw. Only conclude
a precise culprit when a verified before/after pair demonstrates it.


## Phase 5: focused single-draw HDR diagnosis (19, 20, 21)

The preceding on-device capture shows a rapid increase in finite HDR values
above 1000: 0.68% before draw 19, 4.51% after draw 19, and 33.93% before
draw 22. This localizes the main increase to draws **19, 20 and 21**;
it does not yet establish whether source data or shader execution is wrong.

Phase 5 uses only those three draw ordinals in the first two actual HDR
intervals. Each selected draw receives:

- `hdr_before_19/20/21` and `hdr_after_19/20/21` GPU-ordered output images;
- `HDR_DRAW_FOCUS`, `HDR_DRAW_IMAGE`, `HDR_DRAW_CBUF`, and
  `HDR_DRAW_TEXTURE_DESC` records for the corresponding compiled shaders,
  sampled image views, descriptor-referenced CBUF slots and byte extents;
- `HDR_DRAW_BLEND` with the actual attachment-0 blend enabled state,
  channel-write mask, conditional rendering and transform-feedback flags;
- at most **one** `hdr_input_<draw>_<index>` readback per selected draw.
  Eligible image: a non-target, single-layer and single-mip 2D-color
  backing, at most 4 MiB, with a defined layout and TransferSrc usage.
  Descriptor metadata is retained for all other inputs, which are not
  read back. An input might be unavailable due to these restrictions.

The overall 48-image / 144-MiB cap still applies, including tone-map
snapshots. No guest CBUF contents are read. Readbacks split render passes
only after the user manually arms the capture. No renderer correctness
change is made; this commit is strictly an investigation aid.


## Phase 6 — material sources and shader-binary replay after PR #376

PR #376's CBUF used-size propagation did not fix the visible UE4 color
corruption. Phase 5 successfully bracketed the large HDR change in draws
20/21, but its input-selection rule read only a **1x1 3D dummy**. The
resolved descriptor list actually includes BC1/BC3/BC5 materials, BGRA8
scene inputs and an R16G16B16A16Sfloat cubemap with six layers/eight mips.

Phase 6 preserves HDR brackets for draws 19/20/21, and **once per manual
capture**, on the first observed draw 20 with fragment hash
`7161B14045F710B5`, copies the unique resolved sampled backings (up to
32 descriptor indices). Supported images: color images with defined
layout/TransferSrc, no MSAA, depth=1, at most 12 mips / six layers /
20 MiB per image. Compressed BCn image blocks are copied *as blocks*
without host-side decompression. Their linear GPU readbacks are packed
mip-major, with all array layers for each mip; `focus_XX.txt` records
full backing format, mip count and layers. The 1x1 3D dummy is skipped.
Large or unsupported sources log `HDR_MATERIAL_SKIP`.

The targeted fragment shader and its observed vertex partner are cached
in memory at compilation (max 2 MiB per binary) and written to the next
manually armed capture directory as `shader_<HASH>.maxwell.bin` (guest
Maxwell) and `shader_<HASH>.spv` (actual host SPIR-V). Shader caching
handles compilation before arming; **no files** are emitted until the
user explicitly activates the diagnostic. These files enable offline
comparison of guest instructions and Vulkan translation without asking
for more repeated HDR sweeps.

The global focus quota becomes **64 images / 144 MiB** (previously 48 /
144 MiB); writes and Vulkan readbacks are still strictly opt-in. Because
resources can be large, wait for all `MINIRD_UE4_FOCUS wrote snapshot`
messages and include the `shader_*` files when packaging the capture.
This is a diagnostic, not a shader-compiler fix; GPU pass splitting can
perturb only the armed run, and BC blocks require offline decoding.
