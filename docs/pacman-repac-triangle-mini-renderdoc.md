# PAC-MAN WORLD Re-PAC — inverted/giant triangle diagnostic (stage 1)

This diagnostic is **not a graphical fix** and not a full RenderDoc replayer. It
instruments the real Maxwell3D draw paths and the source passed to Android
presentation. It does not alter shaders, index buffers, topology, depth/cull
behavior, resource management, barriers or synchronization.

## Reproduction

The user supplied a 16.6 s gameplay recording (Moto G100 / Strato).
Around 3 s a large dark triangular region crosses the sky and a bright
white clipped region obscures palm trees and the beach. Around 9 s the
same scene has normal-looking sky and beach. This supports a transient GPU
rendering problem; **no particular draw call or source resource is proven**.

## Manual collection

1. Install the diagnostic ARM64 **DevRelease** APK built from this draft PR.
2. Run PAC-MAN WORLD Re-PAC until the beach/wooden-platform scene where the
   artifact reproduces. Keep the game running.
3. In the app's public files directory, create the empty marker
   `gpu_capture/pacman_world/ARM_CAPTURE`. The parent folder is created
   automatically by the presentation thread. On Android 12, use a file
   manager with access to the app's Android/data files or an authorized
   Android/ADB file workflow.
4. Keep the corrupted scene visible for at least six displayed frames.
   The marker is checked approximately every eight host presentations.
5. Find `gpu_capture/pacman_world/capture_###/gpu_events.txt` and
   `capture_complete.txt`; send their ZIP, `logs/emulation.log`, and
   a screenshot/video from the same scene.
6. Repeat on the same scene when **not** corrupted, with identical driver,
   version, Free Guest Texture Memory and readback settings. A/B compare
   draw IDs/order, shaders, culling, winding, topology, depth and targets.

No console commands or modifications to game ROMs are needed for the text
trace. An optional external Android RenderDoc Vulkan layer makes the
already-existing `captureNextExecution` path eligible to capture **one GPU
execution/submission**; the text trace does not require this layer.
No `.rdc` file is promised when the layer is absent.

## Recorded information

- Six host presentation intervals (approximate; not guest frame IDs).
- Presented source texture pointer, width/height, format, Android
  crop/transform and destination swapchain extent.
- Direct, indirect and inline-index Maxwell draws, guest topology and
  count/first/instance/offset, selected pipeline shader hashes,
  rasterizer discard, front-face winding, culling, flip Y,
  primitive restart, depth/stencil test and writes, blending and color mask.
- Scissor, color/depth attachment identity and dimensions, first eight
  sampled image views, full sampled view count.
- At most 12,000 lines of GPU events, with dropped count, and one
  optional RenderDoc-submission request per capture.

The one-shot session automatically stops after six presentations.
No per-frame readbacks, extra render-pass splits, device-wide waits or
diagnostic behavior are enabled outside the arm window.

## Interpretation and next implementation step

1. If both good and bad traces use identical draw state, this does not rule
   out invalid vertex/index data, shader outputs or memory aliasing.
   A second **GPU-ordered before/after draw image and buffer snapshot** must
   target the suspect shader/draw and attachment discovered here.
2. If the bad frame changes winding/cull/depth topology or scissor, trace
   the guest registers and fix the first incorrect conversion, with a
   regression test for the exact state.
3. If color output is already wrong at the scene RT, investigate shader
   execution and/or corrupt geometry. If the scene output is correct but
   presentation is wrong, investigate copy/layout/alias of the source.
   **This stage records metadata, not raw RT pixels, and cannot distinguish
   those last two cases by itself.**

Do not claim a correction or a validated fix before an on-device bad/good
capture and targeted regression test. Keep this PR draft, unmerged.

## Build status

Source integration only; no Android toolchain build or Moto G100 test was
performed when the source was authored. GitHub Actions results should be
checked separately.


## Device capture 001: observed data (2026-11-01)

Files: `capture_001.zip` (959,293 bytes of plain-text GPU events before ZIP)
and `emulation.log`. PAC-MAN WORLD Re-PAC v1.0.2, handheld,
Adreno 650, Qualcomm proprietary 0.777.0, Vulkan 1.1.276.

- Six presentations, 1,580 direct draws, 53 unique shader combinations.
  No dropped events. No indirect or inline draws during this window.
- Two full draw intervals have 395 draws each; a third has the same count.
  Intervals 3 and 4 split one sequence at the asynchronous present boundary
  (184 + 211); **these are not complete independent guest frames**.
- Per complete sequence: approximately 183 depth-only draws to a
  2048×2048 depth target, followed by a 1280×720 scene/color pass and
  postprocessing. The main color target uses Vulkan format 122,\n  `VK_FORMAT_B10G11R11_UFLOAT_PACK32` (packed HDR floating point).\n  This makes scene HDR data worth examining for anomalous pixels,\n  but no pixel values were captured in stage 1. Guest topologies 4\n  and 7, mostly indexed. All
  `flipY=false`; front-face mostly counterclockwise. No evidence of a
  global flip regression from this data.
- Three alternating 1920×1080 VI sources. Source format IDs alternate
  between 37 (R8G8B8A8 UNORM) and 43 (R8G8B8A8 sRGB), with a
  1280×720 crop. This is a potential format identity to compare, **not
  an established cause of the triangle**.
- The RenderDoc layer is absent. Text metadata alone cannot identify
  the first draw producing the bad pixel.
- The log contains an audio latency error unrelated to any verified
  graphics failure; it contains no identified Vulkan failure.

### Stage 2: scene image checkpoints (same Draft PR)

Create an empty `gpu_capture/pacman_world/ENABLE_SCENE_SNAPSHOTS`
marker **before** arming a new `ARM_CAPTURE` marker. Keep the game at the
affected beach scene. As in stage 1, `ARM_CAPTURE` is automatically consumed.

An opt-in readback now samples the actual 1280×720 scene color target
**after** direct scene draws 1, 16, 64 and 176, within approximate
presentation intervals 1 and 2. Only full-resolution 2D, single-layer,
single-mip, uncompressed color RTs with transfer-src and eligible Vulkan
layout are sampled. Maximum eight images, 8 MiB each and 64 MiB overall.
There is no fixed shader hash or game ID in the selection.

This diagnostics-only path ends a render pass for GPU-ordered readback,
which can change scheduling/timing while armed. It is **not a test of
performance or proof of semantically identical rendering under capture**.
Outside manual capture, the code does not touch render passes.

For stage 2, upload:
- The entire new `capture_###` ZIP, including `scene_*.raw` and
  `scene_*.txt`.
- Both `capture_complete.txt` and `snapshot_readbacks_done.txt`:
  the first only closes the metadata window; the latter confirms that
  GPU fence completion and asynchronous file writes have finished.
- The corresponding `emulation.log` and screenshot or video clearly
  showing whether the **captured** scene was bad or good.

Compare per-draw images and earliest divergence; refine the binary
search with new draw ordinals if necessary. No rendering fix should be
committed before evidence ties a specific GPU operation to the defect.

**Stage 2 build and phone execution have not been validated yet.**


## Stage 2 results and Stage 3 (November 1, 2026)

The uploaded second `capture_001.zip` successfully contains six ordered,
post-draw `B10G11R11UfloatPack32` images and a completed async readback
marker. It captures no after-draw-176 image: this title's sampled scene has
101 eligible full-resolution color draws per interval, so ordinal 176 was
unreachable.

The visible triangular discontinuity and overly bright region already
exist after draw 1; after draw 16 they persist; after draw 64 much of the
affected area has been replaced. HDR RGB values above 10 occupy
4.673% / 4.673% / 1.330% of the first interval's pixels, respectively.
The same pattern appears in interval 2. There are no NaN/Inf values.
The 11,264 peak is finite and **does not prove an HDR bug by itself**.

**Stage 3 has been published in the same Draft PR**. The normal
manual triggers are unchanged: create
`gpu_capture/pacman_world/ENABLE_SCENE_SNAPSHOTS` before a new
`gpu_capture/pacman_world/ARM_CAPTURE`. Stage 3 now acquires a
**pre-first-draw** RT snapshot and post-draw snapshots at ordinals
1, 16, 32, 48, 64, 80 and 100, all within one scene interval. It
uses the existing eight-image/64-MiB quota and async readback completion
marker; no diagnostics run unarmed. These probes are intended to separate
old RT contents from the actual draw responsible for a new anomaly.

Send the newly generated `capture_###` ZIP with
`scene_*.raw`, `scene_*.txt`, `gpu_events.txt`,
`capture_complete.txt`, and `snapshot_readbacks_done.txt`,
plus `emulation.log`. A matching phone screenshot/video showing the
visible defect is important to separate expected scene lighting and
raw-image orientation from actual display corruption.

Full numeric results and SHA256 fingerprints:
`docs/pacman-repac-capture-002-analysis.md`.

**Not a rendering fix.** Stage-3 Android compilation and capture
validation are pending. Preserve Draft, do not merge.


## Stage 4: automated opt-in HDR anomaly detection (after third capture)

The third capture produced eight successful 1280×720 GPU snapshots:
one before the first scene draw and seven through draw 100. The earliest
pre-draw image was **clean of extreme HDR values**, whereas the second
capture began with over ten thousand RGB pixels above 100. Only 298 raw
pixels changed between pre-draw and post-draw 1 in the third capture.

To scan across more rendered scenes without requiring exact timing, create
an empty `gpu_capture/pacman_world/ENABLE_HDR_SCAN` file and then
re-create `gpu_capture/pacman_world/ARM_CAPTURE`. The HDR scan marker
takes precedence over `ENABLE_SCENE_SNAPSHOTS` when both exist. It
samples after 1280×720 HDR scene color draw 16, which distinguishes the
previous bad and good runs. It can run for up to 60 host present intervals,
uses no more than 4 in-flight readbacks, and writes up to five RAW frames
(a baseline plus selected severe-HDR matches). Threshold: at least 512
pixels with any R/G/B HDR channel >100, verified against the previous
bad (~10,541) and good (0) samples.

After capture, upload `hdr_scan.csv`, `hdr_scan_done.txt`,
`capture_complete.txt`, `hdr_scan_*.raw`, `hdr_scan_*.txt`,
`gpu_events.txt` and `emulation.log`. Wait for the done marker
before archiving the files. For another fixed-checkpoint capture,
remove `ENABLE_HDR_SCAN` and keep `ENABLE_SCENE_SNAPSHOTS`.

This scan does **not** automatically diagnose a black-only triangle:
it searches specifically for the extraordinary HDR signal present in
the previous bad run. It can affect timing; a clean scan cannot
conclusively eliminate synchronization, shaders or driver causes.
No rendering correction has been made.

Third capture metrics and instructions:
`docs/pacman-repac-capture-003-analysis.md`.
Build and phone execution of this stage remain unverified.
