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
