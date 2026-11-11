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

## Follow-up from real capture_001 (2026-11-11): focused TexMan mode
The first delivered capture completed four VI images plus **20/20 successful** pass readbacks on Turnip 24.0.99. Pink/white glow is already present in the **VI source**, not only the Android swapchain. The intermediate readback budget filled with five 32×32 float surfaces early and then many full-resolution `A2B10G10R10UnormPack32` geometry/G-buffer outputs. Those bright green/magenta G-buffer images are **not evidence of corrupt color**: interpreting packed nonfinal attachments as display colors is misleading. Two `B10G11R11UfloatPack32` 1280×720 HDR passes do contain >1 channels, but that is permitted HDR behavior. The 10,000-event budget was exhausted before the later composition chain was fully traced. **No first bad input or TexMan violation is proven.**

### Second-stage opt-in test (use after the first #445 APK)
Create these empty marker files in `gpu_capture/mk8_glow/` **before** arming:
- `ENABLE_TEXMAN_EVENTS`: a separate, max-4096-event `texman_events.txt` with TexMan lookup choice (create/full reuse/mip reuse/depth-slice/read-only alias), requested guest physical map/format/tile, chosen host texture identity, actual GPU write markers, successful texture uploads/downloads and actual guest-memory free actions.
- `FOCUS_HDR_FULL_RES`: selects only >=512×256 floating-point color render-target snapshots for pass readbacks; excludes 32×32 auxiliary buffers and `A2B10G10R10UnormPack32` G-buffers. Also suppresses repeated draw/pass-target events with no sampled images. It implicitly enables intermediate selection. Standard four-frame VI source readback remains.
- `ARM_CAPTURE`: consumes the markers once and creates `capture_###`.

Do NOT enable legacy UE4 draw-step or boot-logo modes. After completion, ZIP the entire capture folder (especially `texman_events.txt`, `gpu_events.txt`, pass raws and per-pass metadata) and send it with the matching `emulation.log`. Prefer the same Mario Kart 8 start-line scene and Turnip settings. Note whether `Free Guest Texture Memory` is OFF.

**Interpretation:** A TEXMAN `LOOKUP` documents a selected backing, not that its pixels are valid. GPU_WRITE is an ownership declaration, not proof of a completed GPU write. UPLOAD/DOWNLOAD reports a synchronization path, not an automatic data-integrity guarantee. Compare within the **same process** by Texture identities and captured pass/submission sequence; do not compare raw pointers with Eden or a new boot. Only a captured mismatched input and data path justifies a renderer fix.

## Third-stage capture — MK8 postprocess focus

Analysis of the user's second capture showed 16 successful `1280x720 B10G11R11UfloatPack32` snapshots but **all of the same HDR scene texture**. The TexMan trace saturated at 4,096 events (4,015 GPU_WRITE declarations, 71 LOOKUP and 10 UPLOAD). A matching faulty alias, texture corruption, or invalid sRGB operation has **not** been established.

The scene HDR images do not display the conspicuous pink exhaust/white flare visible in the final VI image after approximate visualization. The stage to isolate next is the recorded FS `F8F8901DE46A8AFF`, which composites an HDR scene, 320x180 bloom buffer and 8x8 sampled input to a `1280x720 A2B10G10R10UnormPack32` target. Additional observed FS hashes are `05EB6F31815A3BF8` (bloom upsample) and `AFE30761C7599E71` (640x360 preview and final 1920x1080 presentation).

### Test the new mode

In `gpu_capture/mk8_glow/` remove `FOCUS_HDR_FULL_RES` (the new postprocess mode takes precedence if it is still present), create empty **FOCUS_MK8_POSTPROCESS**, optionally leave `ENABLE_TEXMAN_EVENTS`, and create **ARM_CAPTURE** last. Use the same MK8 starting-grid scene, Turnip 24.0.99, and record Free Guest Texture Memory state. ZIP the entire `capture_###` only after `pass_readbacks_done.txt`; send `emulation.log`.

This mode disables broad HDR pass sampling and instead queues readbacks after the *natural* render-pass endings of three detected stages per approximate presentation interval: `bloom_upsample` (320x180 multi-mip B10G11R11), `hdr_bloom_composite` (1280x720 A2B10G10R10) and `postprocess_preview` (640x360 R8G8B8A8Unorm). The exact observed shader hashes are used only for diagnostics. `pass_XX.txt` records `stage`, `mips`, `layers`, and links to `MK8_STAGE_MARK` in `gpu_events.txt`. Do not parse the complete multi-mip bloom buffer as though it contained only level 0.

The per-pass sample and global byte caps remain unchanged. No forced renderpass boundaries, shader changes, gamma clamps or TexMan behavior fixes are added. TexMan GPU_WRITE declarations are sampled (first 32, then 1/64) so they no longer crowd out lookup events; the total calls and policy are documented in `texman_events.txt`. Report missing stages rather than assuming a root cause.

The same backing pointer appearing as source/destination in lower-resolution bloom draws can represent **distinct mip levels**, and alone is not evidence of illegal feedback. Keep the PR Draft, run CI and test the APK before contemplating a separate minimal production fix.
