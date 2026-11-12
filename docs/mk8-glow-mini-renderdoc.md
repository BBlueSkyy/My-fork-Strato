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


## Focused bloom-mip readbacks (new: fourth-stage diagnostic)

This mode is **experimental, opt-in and separate from the existing wide mini-RenderDoc captures**. It does not correct graphics. It is intended to test *content*, not the now-disproved same-mip feedback hypothesis: PR #449 showed TSC clamps 4, 3, 2 for writes to mips 3, 2, 1 respectively.

1. Install the #445 ARM64 DevRelease APK, leaving your main Strato untouched. Use MK8 Mario Kart Stadium with the same Turnip driver and **Free Guest Texture Memory OFF**.
2. Under the application's public `gpu_capture/mk8_glow/` folder, delete older capture markers `FOCUS_MK8_POSTPROCESS`, `FOCUS_HDR_FULL_RES`, `ENABLE_INTERMEDIATE_TARGETS`, and `ENABLE_UI_DRAW_STEPS`. Create an empty marker named **`FOCUS_MK8_BLOOM_MIPS`**.
3. Create **`ARM_CAPTURE`** last. Allow four presented frames while the relevant starting-grid scene is visible.
4. In the new `capture_###`, wait for **`pass_readbacks_done.txt`**. Expect `requested=9, written=9, failed=0` *if all three 3→2→1 bloom passes occurred while armed*. If less, send what was captured rather than repeating without inspecting the log.
5. ZIP the capture folder and send it together with `emulation.log` and a screenshot.

Nine tiny `bloom_mip_XX_dstN_mipM_{before_source|before_target|after_target}.raw` files (three per output mip) plus paired `.txt` metadata contain tightly packed **single-mip** Vulkan `B10G11R11UfloatPack32` data, at 4 bytes per pixel. They do not contain full multi-mip images, PNG or RDC.

The selected source is mip N+1 and target is mip N for output mips N=3,2,1. Each triplet includes source-before, target-before and target-after. The snapshots are inserted before/after the *existing natural renderpass* using `InsertPreRpCommand` and `AddPostRpCommand`; no forced split, no shader, LOD, guest memory or blending changes. Three triplets maximum and shared callback/fence completion accounting. **These bracket a render pass, not necessarily one draw.** Readbacks can perturb frame timings while armed. Keep this draft unmerged.


## Fifth-stage follow-up: actual fragment CBUF and blend constants

The previous 9/9 valid per-mip snapshots showed byte-identical chaining of bloom upsample outputs, while RP25's first 40×22 draw yielded output very close to destination RGB times constants (roughly R=0.1506,G=0.2646,B=0.5). That may be **intentional** if the fragment shader's RGB multiplier is zero.

The SPIR-V FS 05EB6F31815A3BF8 names the relevant block `c3_f_4` (fragment-stage CBUF 3), with a float array stride of 4 and indices 24,25,26: byte offsets 96,100,104. In each focused `BLOOM_MIP_PASS` record we now log:
- `dynamic_blend_rgba=...`: current Maxwell blending-constant registers corresponding to destination factor CONSTANT_COLOR.
- `fragment_cbuf3_f24_26=...` and `fragment_cbuf3_bits=...`: actual float multiplier values and exact bits, when bound, large enough, and not current-execution GPU-dirty. Otherwise `cbuf_status=skipped`; never silently substitute zero.

No shader, LOD, sampler, TexMan, blend, render-pass boundary, or snapshot-count changes. Capture only on the existing opt-in `FOCUS_MK8_BLOOM_MIPS` mode. Use `ARM_CAPTURE` at the same MK8 stadium scene and send the full new `capture_###` ZIP, `emulation.log`. Keep Draft; do not merge.


## Sixth-stage proof: input vs output of the HDR bloom compositor

The bloom upsample capture validated actual shader uniform scales and blend factors, and its three consecutive mip stages were coherent. The preexisting stage montage already shows the oversized pink/white glow **at the 1280×720 HDR-composite output**, prior to preview/final presentation. This does not prove whether the shader receives bad input or produces bad output.

To isolate those two possibilities, repeat the existing **FOCUS_MK8_POSTPROCESS** mode (not \`FOCUS_MK8_BLOOM_MIPS\`):
1. Under \`gpu_capture/mk8_glow/\`, remove \`FOCUS_MK8_BLOOM_MIPS\`, \`FOCUS_HDR_FULL_RES\`, \`ENABLE_UI_DRAW_STEPS\` and \`ENABLE_INTERMEDIATE_TARGETS\`. Create the empty \`FOCUS_MK8_POSTPROCESS\` marker.
2. With MK8 Mario Kart Stadium starting grid visible, same driver / Free Guest Texture Memory OFF, create empty \`ARM_CAPTURE\` last.
3. Wait for the new \`capture_###/pass_readbacks_done.txt\`. ZIP the entire capture and send it with \`emulation.log\` and a screenshot.

The stage now records, on the **same process and natural composite RP**, the source **1280×720 R11G11B10_UFLOAT HDR scene** and **320×180 R11G11B10_UFLOAT bloom chain**, immediately *before* the composite render pass, plus its already-supported post-RP **1280×720 A2B10G10R10_UNORM** output. Each input RAW file is named \`pass_XX_composite_input_{scene_hdr|bloom_hdr}.raw\`; each matching \`pass_XX.txt\` has \`stage=hdr_bloom_composite_input_...\`, the sampled image-view mip range, texture identity, format, total bytes, and \`mip_layout=\` entries. Decode bloom \`mip0\` from the buffer offset in its metadata rather than treating its **entire five-mip packed RAW** as a single image.

This adds **two opt-in source copies per composite stage**; the existing pass readback quota is five per interval (one bloom output, two new compositor inputs, one compositor output and one preview output), up to 20 total. Each copy occurs *before* the natural compositor render pass and all writes use the existing callback/fence completion scheme. No shader, LOD, TexMan, blend, guest memory or forced render-pass split is changed. If \`requested\` is less than 20, inspect \`MK8_COMPOSITE_INPUT_SKIP\` in \`gpu_events.txt\` and send the partial capture; do not infer missing resources were zero. The four-presentation window is approximate. The diagnostic itself can perturb GPU timing.

The goal is to compute the compositor's true source and output pixel values, then compare them against the shader math or a matched historical pre-#341 capture. An excessive source, excessive composite output, or stale layer/subresource **must be demonstrated before any production change**. Keep this Draft unmerged.


## Seventh-stage capture — provenance of the anomalous bloom source (PR #445)

Three full 15/15 compositor captures have now established: the 320×180 five-mip bloom read immediately after upsample is **bitwise identical** to the sampled bloom input to the HDR compositor, and the compositor's 1280×720 output carries the exaggerated pink/white streak. The 1280×720 HDR scene input itself does **not** show the same streak. The bloom input has strong magenta/blue in the lower-right racing surface and weak spatial overlap with HDR scene bright pixels at those same coordinates. This is suspicious but **not yet proof of corrupted data**: the game can compose emission/highlight buffers that differ from the final HDR scene.

GPU event chronology provides the next unique upstream link: shader **3F6EAFE7F26D6292** generates the 320×180 bloom mip0 by sampling a **separate 640×360 B10G11R11UfloatPack32** HDR image plus a 1280×720 R16Sfloat image. No previous capture contained the 640×360 input. The follow-on 3784BE1F / 77260D2B / 69575297 stages generate its other four mips; 05EB6F31 upsample stages are already numerically validated.

The latest build therefore adds **exactly one** additional input readback per approximate frame, when FS=3F6EAFE7F26D6292 and the target is 320×180/mip0:
- file: \`pass_XX_bloom_origin_640x360.raw\` (B10G11R11UfloatPack32, 640×360×4 bytes).
- metadata: \`pass_XX.txt\` with role \`bloom_origin_input\`, image identity, producer render pass and submission.
- GPU event: \`MK8_BLOOM_ORIGIN_INPUT\`, or \`MK8_BLOOM_ORIGIN_SKIP\` if quota is exhausted.

The source copy is recorded immediately before the producer's *natural* render pass, with existing fence-completion writer. The output mip0 is **already** captured in the existing \`pass_XX_composite_input_bloom_hdr.raw\` at offset zero (230400 bytes), after all downsample/upsample stages. No additional mip or compositor readback is needed. The per-interval quota increases from 5 to 6, global from 20 to 24; this changes **only the maximum number of diagnostic snapshots**, not game rendering. No forced RP split, shader, LOD, blend or TexMan modification. All captures remain manual opt-in and timing-sensitive.

**Phone test:** use the updated #445 APK and exactly the previous \`FOCUS_MK8_POSTPROCESS\` + \`ARM_CAPTURE\` markers at the same MK8 Stadium scene, Free Guest Texture Memory OFF. Wait for \`pass_readbacks_done.txt\`, ZIP capture folder and send it with \`emulation.log\`. Expect up to **18** snapshots for 3 complete processing frames (six each). Compare prefilter 640×360 with 320×180 mip0 and later composite output. If the source is already anomalous, trace its producer rather than altering the upsample or compositor. A same-scene pre-#341 golden capture still required to attribute the regression conclusively. **Keep Draft, no merge.**


## Eighth-stage metadata-only alias verification (640x360 HDR)

The user capture on #445 e76d07c5 (24/24 readbacks) revealed a specific stale-source hypothesis:

- All four **640x360 bloom-origin** raw files (\`pass_05\`, \`06\`, \`12\`, \`18\`) have **identical SHA-256**, despite the four 1280x720 scene HDR files changing by 93–97% of 32-bit pixels between snapshots. The source raw is finite but has very strong highlights (max R/G/B 14.75 / 16.5 / 17).
- The bloom 320x180 mip0 is strongly spatially correlated with the origin and all 1,378 >1 bloom pixels in matched frames fall within the origin's >1 pixel mask after 2x2 reduction. The bloom values can thus originate before upsample/composite.
- The bloom-origin 640x360 Texture was **sampled five times and never appeared as any draw destination** in the bounded 4-presentation GPU-event log, while a **different** 640x360 HDR Texture was written **15 times**.
- This raises a concrete hypothesis of a stale 640x360 cache/alias binding after #341. It is *not yet a proven bug*. A static texture may be intentional, and guest-memory relationship between the sampled and actively-written representations is unknown.

This diagnostic addition therefore records **no more image data**. During the existing \`FOCUS_MK8_POSTPROCESS\` + \`ARM_CAPTURE\` capture, for only these observed fragment shaders:

- \`3F6EAFE7F26D6292\`: 640x360 bloom-origin sampled Texture.
- \`42016CE58CA1A259\` and \`AFE30761C7599E71\`: any 640x360 B10G11R11 HDR render targets.

The new \`MK8_640_ALIAS\` event records the Texture identity, all guest memory mapping spans, alias group pointer, group writer identity/generation when obtainable without blocking, and stale/retired/read-only flags. It takes no GPU readbacks, adds no render-pass boundaries, does not alter descriptors, mips, shaders or TexMan. The alias-group mutex is try-locked; missing group state is explicitly marked unavailable rather than fabricated. Only compare pointer/range identities **within the same game process**, not across reboots. The original 24-pass capture remains available, but only \`gpu_events.txt\` from a new \`capture_###\` is necessary to examine the metadata.

**Interpretation:** exact or overlapping guest spans plus stale source + newer writer could justify a pinpoint change to alias reconciliation/mip backing selection. Non-overlapping spans would redirect to producer lifecycle (and may be intentional). Only after verifying this—and ideally a matched pre-#341 control—should the user receive a Work prompt to implement a production fix. Keep Draft, no merge.


## Ninth-stage correction: prove APK revision and log guest-less 640x360 images

The most recent device capture has **24/24 successful HDR readbacks** and identifies the same frozen 640x360 source in all four frames, but **zero MK8_640_ALIAS events**. This is not event-buffer overflow: \`gpu_events.txt\` contains only ~4,438 lines, below the 10,000-event cap, and the specific shader 3F6EAFE7F26D6292 and writer 42016CE58CA1A259 do execute in those frames. The earlier diagnostic incorrectly returned silently for \`!tex->guest\`. A *different, older APK installation* is also possible because the capture log does not include a build-specific marker.

Fix the **diagnostic**, not rendering:
- On arming \`FOCUS_MK8_POSTPROCESS\`, emit exact marker \`MK8_ALIAS_PROBE_REV=2 guestless_fallback=true\` in BOTH \`gpu_events.txt\` and \`emulation.log\`.
- Emit \`MK8_640_ALIAS\` for any 640x360 object reached via bloom-source FS 3F6EAFE7F26D6292 or candidate writers FS 42016CE58CA1A259 / AFE30761C7599E71, **even if guest mappings are missing** or format/depth differ. Capture \`guest_present\`, mapping spans, dimensions/depth, format, mip count, alias-group pointer, and nonblocking writer metadata when available.
- The output can now distinguish (a) older APK not having the revised probe (no revision marker), (b) valid host-only texture with no guest memory or alias group (\`guest_present=false\`), and (c) guest-backed texture with matching/overlapping physical spans.

No new Vulkan commands, GPU readbacks, altered resource ownership or render-state changes. Same \`FOCUS_MK8_POSTPROCESS\` + \`ARM_CAPTURE\` test. **Send only \`gpu_events.txt\` and \`emulation.log\`**. Do not claim a proven #341 alias bug or produce a Work fixing prompt until the guest-memory relationship / actual writer is established. Do not merge.
