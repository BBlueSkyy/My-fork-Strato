# Dragon Ball: Sparking! ZERO — mini-RenderDoc (Strato)

This is a manual opt-in trace built from the first-stage UE4 capture in PR #375. The existing PR #377 history is retained and refreshed onto master at `68f4f3ab` (including merged PRs #383 and #385). It does not produce a RenderDoc .rdc file or itself fix graphics or DEVICE_LOST.

## Capturing on the Moto G100

1. Install the ARM64 **DevRelease** APK from the draft PR.
2. Launch Dragon Ball: Sparking! ZERO and enter a fight where the arena is blue/black or green/black.
3. Open public Strato files at `gpu_capture/sparking`. Rename `ARM_CAPTURE.rename_this_file_when_ready` to exactly `ARM_CAPTURE`, or create an empty file with that name.
4. Return to gameplay. The trigger is checked at least once every 16 presented frames. When `capture_001/capture_complete.txt` appears, check `emulation.log` for any pending asynchronous `MINIRD_SPARKING wrote snapshot` messages before copying the files.
5. Send `capture_001.zip`, the matching `emulation.log`, and a screenshot of the bad arena.

## Files

- `present_00..05.txt` and `present_source_00..05*.raw`: six source color images read *before* Android presentation.
- `pass_XX*.raw` and `pass_XX.txt`: full/half-resolution sampled and render-target color images, format/backing metadata, and render-pass relationships. These are raw linear staging bytes, not PNGs.
- `gpu_events.txt`: up to 12,000 draw/shader hash/texture binding events, texture mapping and GPU transfer diagnostics.

Budgets: six **approximate** presentation intervals, up to 6 pass snapshots per interval, 30 total, 160 MiB total pass-image bytes and 20 MiB per eligible image. Because rendering is asynchronous, presentation interval is not an exact guest frame or draw index. Readback while armed may change timing, so compare gameplay with capture disabled too. Inspect the first incorrect pass and compare sampled input and output before blaming a shader or descriptor. This diagnostic branch is **Draft**, not a rendering fix.

## Phase 2 — focused arena/HDR path (capture_002 onward)

The first device capture wrote all 30 snapshots, but they were 1024x256/512x512 auxiliary resources. `present_source_00` already showed the corrupted arena (1280x720), and the draw log established the 856x480 path. Phase 2 now captures GPU-ordered **render-pass-end** images from:

1. `scene_hdr`: up to two native 856x480 B10G11R11 producer targets per approximate present interval;
2. `tonemap_hdr_input` and `tonemap_ldr_output` for fragment shader `3332897FCDBD1264`;
3. `post_ldr_input` and `post_ldr_output` for `96D28FD4260951CB`;
4. `upscale_ldr_input` and `upscale_ldr_output` for `2CFA19C0057B1932`.

The precise texture role and target fragment shader hash are included in each `pass_XX.txt` and `PHASE2_SNAPSHOT` event. Global budget remains 160 MiB, with at most 8 role-limited snapshots per approximate interval and 48 total. Other formats/resolutions are skipped, preventing auxiliary saturation.

As with the initial implementation, these are after-pass snapshots, **not** exact pre/post-draw image pairs, and GPU work may lag the approximate present index. Readbacks are opt-in only. If no scene targets appear because dimensions, format, transfer usage or shaders differ in another fight, inspect the DRAW/FOCUS_BINDING metadata instead of assuming a stage is valid. The independent `VK_ERROR_OUT_OF_POOL_MEMORY` warnings remain unresolved; do not attribute corruption to them without evidence.

Capture instructions and trigger are unchanged: create `gpu_capture/sparking/ARM_CAPTURE` when fighting, then collect the next `capture_###` folder plus `emulation.log`. Verify pending asynchronous pass writes finish before zipping.


## Comparing different boots on the same base

Use the ARM64 DevRelease APK of this refreshed PR #377 for both runs. The update
preserves the phase-2 capture and current master shader-compiler pin; it is not a
fix for arena corruption. No need to arm the Cosmic Shake HDR marker.

1. Start a fresh app process and enter the same arena, with the same fighters,
   graphics settings and driver. Once the corruption is visible, arm
   `gpu_capture/sparking/ARM_CAPTURE`.
2. Wait for capture completion and pending asynchronous snapshot writes as above.
   Save the complete capture folder, matching `emulation.log` and a screenshot
   together, labeled with the observed result (for example `green`).
3. Close the app completely, relaunch the same APK and repeat when a different
   result appears (for example `black` or `magenta`). Preserve both captures;
   their folder numbers alone do not identify the boot or APK version.
4. Compare the `scene_hdr`, tone-map, postprocess and upscale roles by dimensions,
   format and shader, not by host pointer or physical address across processes.
   Include the APK commit and driver version with both captures.

These are still after-pass readbacks, not exact before/after snapshots of one
draw. They can locate the first captured stage that differs, but cannot alone
prove which descriptor, source texture or constant-buffer value caused it.
Capture overhead can also affect timing; performance comparisons should use
capture disabled. Android compilation and this refreshed capture need device
validation on the Moto G100. No mandatory build tests were added.
