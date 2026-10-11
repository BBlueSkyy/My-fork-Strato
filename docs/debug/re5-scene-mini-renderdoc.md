# Resident Evil 5 — bounded mini-RenderDoc (diagnostic only)

Based on master 140dae65115cddc1289ab222b27faef18f66d83d and the Naruto UI diagnostic capture. This is **not** a RenderDoc `.rdc` and is **not** a rendering fix.

## Observation from the supplied 43s clip

Resident Evil 5 Chapter 1 gameplay shows dark character and crowd silhouettes against a bright scene, and some suspect sharply black rectangular regions on/near buildings. The original-game comparison and exact first defective draw are not yet established. UI/text appears visible.

## Usage

1. Install the diagnostic Android APK and enter gameplay when the defect is visible.
2. In Strato public app files, rename `gpu_capture/re5_scene/ARM_CAPTURE.rename_this_file_when_ready` to `ARM_CAPTURE`.
3. Resume gameplay and allow four consecutive presentations to be captured; the initial frame might already have been rendered. Arming polls every 16 presentations.
4. Wait for `MINIRD_RE5 capture complete` and subsequent `MINIRD_RE5_PHASE2 wrote snapshot` messages. `capture_complete.txt` alone is not proof that asynchronous staging readbacks have finished.
5. Zip the latest `gpu_capture/re5_scene/capture_###` folder with the emulation.log and a matching image or video; report Vulkan driver version.

## Outputs

- `present_source_XX_*.raw`, `present_XX.txt`: actual host GPU source image, before Android swapchain copy, and associated host format/crop/guest-map.
- `pass_*.raw`, `pass_XX.txt`: post-renderpass intermediate Vulkan color images with submission, renderpass, texture identity, host format, approximation of presentation interval and metadata.
- `gpu_events.txt`: direct/indirect/indexed draws, guest shader hashes, RT identities and formats, depth-attachment identities, blend masks and flags, scissor, sampled view metadata, unmapped TIC events.
- Limits: 4 presentations, 2400 events, 12 post-pass snapshots per approximate presented interval, 48 snapshots, 160MiB total intermediate bytes.

## Interpretation

- If `present_source` is correct but the screen is not, investigate swapchain/crop/transform. If already wrong, inspect post-pass images and find the first incorrect pass.
- Match suspicious pass by renderpass index, submission and attachment identity, then associated shader hash and sampled textures. Draw events and readbacks are *not* 1:1: a readback is at end of renderpass. `present_index_approx` is approximate, not a frame-accurate guest GPU fence.
- Distinguish genuine backlighting from incorrect material/fragment/depth state by comparison to expected native Switch or Eden output at the same camera and moment.
- For a defect still unresolved, perform a second probe filtered by discovered shader/attachment; do not apply a title-specific rendering workaround.
- This is opt-in only. The readback introduces possible GPU timing overhead; no guest RAM or functional render state changes. Depth is logged, not copied in this first capture.
