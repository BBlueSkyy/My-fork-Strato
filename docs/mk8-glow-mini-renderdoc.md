# Mario Kart 8 Deluxe: pink/white glow mini-RenderDoc (Draft)

## Purpose and reference
On the Moto G100 / Adreno 650, the Strato capture shows unusually bright magenta/pink bloom behind Mario's kart and clipped white glare near the lower screen edges during the MK8 Deluxe starting-grid sequence. The comparative Eden screenshot is much more controlled. The captures do **not** prove whether the difference originates in a shader, render-target format, sRGB handling, blending, texture alias, or GPU submission ordering. Driver, shader cache, settings and game version must be matched before interpreting A/B results.

This draft ports the bounded opt-in mini-RenderDoc diagnostic from closed, unmerged PR #386 onto current master and adds MK8-specific intermediate-color-target selection, shader hashes, render-target identities and packed blend state. The normal renderer's color output and shader code are **not** changed. It is **not** an official RenderDoc .rdc capture.

## How to capture on Android (no PC or root)
1. Install the **ARM64 DevRelease** APK from this Draft PR.
2. Open MK8 Deluxe, position Mario on the starting grid where the pink rear glow and white flare are visible. Remain there with the race countdown visible when possible.
3. In Strato's public app files create/open `gpu_capture/mk8_glow/`.
4. Create an empty file named **ENABLE_INTERMEDIATE_TARGETS** in that directory. This includes post-processing/color targets (as small as 32×32) instead of only confirmed VI overlaps. It is more informative for the bloom question but may consume the snapshot budget before late draws.
5. Create an empty file named **ARM_CAPTURE** (no extension). The emulator polls every ~16 presentations and removes the marker once consumed.
6. Leave the game running for at least four actual presentations. Wait until the new `capture_###/pass_readbacks_done.txt` exists. Check `requested`, `written` and `failed`. `capture_complete.txt` is NOT proof that asynchronous pass readbacks completed.
7. ZIP the **entire** `capture_###` directory using an Android file manager and send that ZIP, `emulation.log`, screenshot, game update/version, driver, and the **Free Guest Texture Memory** ON/OFF value.

For a lighter reference capture, omit ENABLE_INTERMEDIATE_TARGETS and repeat with the same scene/settings. This variant prioritizes targets mapped to VI/presented images. The legacy `ENABLE_UI_DRAW_STEPS` and `ENABLE_BOOT_LOGO_TRIGGER` options were originally specialized for UE4 and **should not be used for the initial MK8 capture**.

## What the archive contains
- `present_source_XX*.raw` and `present_XX.txt`: original GPU VI source *before* Android swapchain presentation; dimensions/format/layout/guest mapping.
- `pass_XX*.raw` and `pass_XX.txt`: GPU-ordered color render-target snapshots at render-pass boundaries; their metadata include size, format, approximate presentation number, executor, submission, render pass and target identity.
- `gpu_events.txt`: bounded records for shader hashes, sampled target identities/formats, color-target identities/formats, packed Vulkan blend factors/ops/write masks, scissor/viewport, render target size adjustments and Fermi blits.
- `pass_readbacks_done.txt`: asynchronous writer completion counts.

**.raw is not a PNG or .rdc.** Decode only using each corresponding .txt format, dimensions and image layout; do not assume all raw images are RGBA8. A RenderDoc capture/replay is not included in this PR.

## Analysis plan
1. Compare present_source and the affected screenshot: if the pink/white blowout already exists in VI source pixels, the Android presentation copy is not its first origin.
2. Find the first captured pass where the white/magenta intensity becomes incorrect. Walk back through shader hashes, sampled input identities and blend state, and compare pass/submission relationships.
3. Separate possible HDR/sRGB format/transfer mismatch from additive blending, shader values and stale texture ownership. Confirm the claim with an unchanged-driver A/B (Free Guest Texture Memory OFF/ON if needed).
4. Implement the smallest evidence-backed fix **in a follow-up PR**, with representative MK8, SMO and BOTW regression tests. Avoid artificial brightness clamps.

## Limits and safety
Manual opt-in only; capture incurs synchronous GPU readbacks and may affect frame timing, frame ordering or trigger the very bug under investigation. Four approximate VI frames, 5 pass snapshots per interval (20 max), 96 MiB pass total, 12 MiB per pass and 10,000 event records. The first five qualifying passes in an interval may not include the bloom writer; a follow-up retargeted capture may be required. Source snapshots are capped independently. The guest image mapping, shader compiler, GPU texture management and normal production render behavior are unchanged. Keep Draft; do not merge until verified on-device and reviewed for stability.
