# Resident Evil 5 — selective scene mini-RenderDoc (phase 2)

Draft PR #337, diagnostic only; **do not merge** as a rendering correction.

## Evidence from the actual gameplay capture

The RE5 gameplay capture contained four 1920x1080 pre-swapchain images, 36 color snapshots, and 2,388 sampled draw records; it exhausted 2,400 event slots in approximate presentation interval zero. Most snapshots were collected before the main 3D color target could be followed sufficiently.

Within those records:
- One 1280x720 color target was written by 886 draws and sampled another 1280x720 render target.
- The other 1280x720 target was written by 259 draws.
- Both use the same 1280x720 D24/S8 depth attachment.
- Frequently observed fragment hashes include `F3805B306B15F89A` and `B141A1171435D55E` on the main color path; `8E1C37D501C478E1` and `07C66B919FC55A45` on the upstream color path.

User confirmed the sky is normally fine; character models and parts of the ground are black. This is **not** proof those four shaders are defective, nor that an observed abnormal sky frame is a real rendering failure rather than a diagnostic timing side effect. No Title ID or pointer-specific rendering workaround is used.

## What changed

- Track post-pass color readbacks **only** from 1280x720 color images used with a 1280x720 depth attachment, not every sampled color texture or early postprocessing pass.
- Continue capturing source images immediately before Android swapchain copy.
- Record sparse `SCENE_DRAW` entries with shaders, sample textures, color/depth identity, scissor and blend state. Prefer four hashes found in the actual first capture, but still sample draws using other shaders to avoid falsely restricting the diagnosis.
- Per-approximate-present sampling: priority draws (first 8, then one per 32), other scene draws (first 4, then one per 128). This prevents the first busy interval from filling the event log.
- Budget: up to 6 pass readbacks per approximate present interval, up to 24 overall, 100 MiB of intermediate data, 2,400 total logged events. Capture frames remain four, with unique globally reserved snapshot IDs.
- Save depth attachment **identity**, not depth pixels. No new barriers, pass splits, stalls or guest state changes are introduced in the scene draw path.

## How to capture

1. Install the diagnostic APK of the updated PR #337 after its build succeeds.
2. Enter gameplay when blackened characters and black ground patches are visible.
3. Rename `gpu_capture/re5_scene/ARM_CAPTURE.rename_this_file_when_ready` to `ARM_CAPTURE` in the Strato public app files. If the rename hint has not been created, enter gameplay and let a few presentations occur.
4. Resume gameplay and remain at the same location for 10–15 seconds.
5. Include the new `capture_###` folder, `gpu_events.txt`, `emulation.log`, a screenshot, and the Vulkan driver version. Wait for `MINIRD_RE5_PHASE2 wrote snapshot` messages before zipping; `capture_complete.txt` only reports that presentation acquisition finished.

## Interpretation and limitations

- The source image is read **pre-swapchain**. Compare it against post-pass color snapshots.
- `SCENE_DRAW` is sampled, not a full draw trace. A shader hash being present does not prove which pixels it affected.
- Snapshots are taken **at natural Vulkan render-pass ends**, not immediately before/after individual draw commands. Copying an image while a render pass is active is invalid without changing the rendering path; forcing pass splits is inappropriate for the first targeted probe.
- `present_index_approx` is not a strict GPU frame identifier. Use submission, render pass, texture and depth IDs to correlate.
- If upstream color is already wrong before sampling by a downstream shader, examine the upstream RT and its sampled/depth dependencies. If upstream looks right but downstream is black, inspect the downstream fragment shader input state or pipeline.
- First capture may be affected by GPU readback timing. Repeat only if results are inconsistent, not as a reflex.
- Once a **specific** problematic draw/pass is identified, add a temporary deeper probe in the same diagnostic branch if needed. Do not apply speculative Texman/shader/depth changes.

## Optional header-only host test

`g++ -std=c++20 -Wall -Wextra -Werror -I app/src/main/cpp tests/gpu/re5_scene_capture_tests.cpp -o /tmp/re5_scene_capture_tests && /tmp/re5_scene_capture_tests`
