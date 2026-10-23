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


## Phase 3 — opt-in HDR draw scan

The two device captures show nonfinite scene-HDR pixels before tone mapping.
The first capture has 223,831 NaN pixels in `pass_01`; the second reaches 271,012
in `pass_33`. These are after-pass observations, not proof of a particular draw
or source. Phase 3 narrows that interval without changing guest shader code.

1. Install this PR's ARM64 **DevRelease** APK. In `gpu_capture/sparking`, create
   the empty file **`ENABLE_HDR_DRAW_SCAN`** and fully restart the app before
   launching the game. The startup marker also enables bounded shader dumps.
2. Enter gameplay with visible corruption, then create/rename **`ARM_CAPTURE`**
   in the same folder. The Cosmic Shake `ENABLE_HDR_FIRST_BAD` marker is unused.
3. After capture completion, wait for pending `MINIRD_SPARKING_DRAW wrote ...`
   log messages to finish. Send the new `capture_###` folder, the matching log,
   a screenshot, **and the sibling `scan_shaders` folder**.
4. Remove `ENABLE_HDR_DRAW_SCAN` and restart to return to the existing phase-2
   pass capture. Remove the marker and leave capture unarmed for normal play.

The scan follows the first eligible 856x480 R11G11B10 HDR backing on one executor.
It saves a baseline before its first selected draw, then a snapshot after each
of up to **192 draw calls**, while the existing six-present capture is active.
Inline and indirect paths participate; one indirect API call may contain several
GPU draws. The baseline may already contain corruption if armed mid-frame.
`hdr_draw_stats.csv` records ordinal, snapshot name, NaN pixel count and infinity
pixel count. The same pixel can contribute to both counts if different channels
are nonfinite. There is no separate before-image for every draw: clears, other
writers/executors, skipped targets and conditional/indirect execution still need
trace correlation. Fixed dimensions and the single target can miss other paths.

`HDR_SCAN_DRAW` correlates hashes/parameters; `HDR_SCAN_INPUT` records up to 32
sampled views with format, mip and layer selection. First-use input backings are
copied once, at most 64 backings and 48 MiB. They are not guaranteed fresh for
later draws; depth, 3D, multisampled, oversized or unsupported inputs are skipped.
CBUF records contain descriptor bindings and declared/used sizes **only**, not
CBUF contents. This avoids CPU reads of GPU-dirty buffers or introducing waits.

`scan_shaders` contains guest and emitted/replacement-selected SPIR-V binaries
seen during compilation, capped at 2048 files / 64 MiB per process. Filenames use
decimal guest hash and FNV-1a content hash; `shader_manifest.txt` maps them. More
than one runtime variant may share a guest hash; hashes alone do not prove the
exact module variant used by a captured pipeline. Already cached shaders may
not be emitted again, so enable the marker before a fresh app process.

Raw scan allocations are capped at **384 MiB** including inputs. The phase-2
pass snapshots are disabled in this mode; the six presentation sources remain.
Render passes are split around diagnostic image copies and an image-specific
transfer-read dependency orders later writers. Copies remain in the existing
executor stream, without a new explicit submit, fence wait or device-idle call.
Readbacks, disk writes and pass splits can materially alter timing and memory
use while armed. CPU statistics/file writes run through completion callbacks.
An incomplete/truncated scan or a timing-dependent disappearance is not a fix.
Android compilation, device behavior and whether the first anomaly fits inside
the selected window still require validation on the Moto G100.

Host check: `python3 tests/gpu_sparking_scan/run.py` (ASan/UBSan; Vulkan and fmt
headers required for the extracted production-function facade). It exercises
opt-in/default behavior, target/executor selection, quotas/reset, packed HDR
nonfinite decoding and production helper metadata/readback scheduling. The
facade does not execute Vulkan commands or validate a real GPU render-pass split.
No test was added to APK workflows.


## Phase 4 — first-draw cubemap upload provenance

The phase-3 device capture contains all 192 draw snapshots. Its baseline has
zero NaNs, then draw 1 (fragment hash `87D698D84285F36B`) has 22 NaN pixels.
The first-use sampled input 14 is an FP16 HDR cubemap, 128x128, six faces and
eight mips, already containing NaNs in over 89% of texels. Its captured SPIR-V
samples that cube and the sampled RGB feeds the color-output calculation.
This identifies a corrupt sampled input, not the stage that corrupted it or
proof that this input explains every visual defect in the game.

The existing **ENABLE_HDR_DRAW_SCAN** startup marker now also enables retention
of the actual CPU upload path for this cubemap geometry. No new marker is needed:
install this updated APK, fully restart with the marker present, enter gameplay
and arm **ARM_CAPTURE** as before. Send the new capture folder and matching log.

For the captured cubemap input, `*_provenance_guest.raw`, `*_provenance_linear.raw`
and `*_provenance_upload.raw` retain respectively the guest mirror, bytes directly
after deswizzling, and actual staging source before the Vulkan upload. The
`*_provenance.txt` report includes mappings, format/tile configuration/stride,
48 face/mip offsets, CRC32 and first differing byte against the already captured
GPU readback. Linear/upload data uses mip-major packing across all six faces;
guest bytes retain the game's layer stride and padding.

Proof retention is enabled before game loading and bounded to **32 MiB of live
CPU payload**. Each candidate is bounded to 4 MiB guest and 2 MiB per linear/upload
stage. The sampled proof is chosen after attached textures perform pending host
uploads and before the record slot is published, while texture locks are held.
No guest data is reconstructed or read at the later readback callback. A matching
staging identity and byte comparison mark that the upload copy was recorded.
Source CPU snapshots are sequential, not an atomic snapshot against concurrent
guest writes. Retained copies do not modify the original source buffers.

Missing proof is reported explicitly (disabled, not observed, unsupported layout,
format/geometry mismatch, unavailable/oversized guest bytes, or budget rejection).
The report also includes sync/copy/discard counts, backing generation, GPU-writer
history and alias/retirement state. Write serials are **CPU write claims**, not
GPU-completion identifiers: equal serials do not rule out a pending writer, and
unequal upload/readback bytes alone do not prove a Vulkan fault. Image writers,
alias reconciliation or backing promotion may legitimately occur after upload.
If the cube is produced only on the GPU, this CPU-upload proof may be absent.

This adapts the existing Cosmic Shake upload-retention diagnostic to the Sparking
cube; it is not a texture-layout or rendering correction. It reuses the existing
input readback: no new GPU readback, upload, fence wait, or device-idle call is
introduced. With the marker absent the CPU payload is not retained. No workflow,
shader-compiler submodule, BufferManager or GMMU changes were made.

Validation: focused ASan/UBSan policy/proof comparison and extracted helper tests;
existing mapping and submission regressions; complete production upload/deswizzle
methods exercised on a synthetic 128x128 FP16 cubemap under Vulkan/UBSan with
48 face/mip slices, matching guest/linear/staging/readback bytes and zero Vulkan
validation errors (both image-format-list settings). The host Vulkan fixture
uses allocator/scheduler facades and llvmpipe; it does not replace the Android
build or Moto G100 validation. The previously documented broad alias-suite
baseline failure remains outside this focused check. Tests remain opt-in and
were not added to APK workflows.

### Cubemap origin before the retained upload

With the same startup `ENABLE_HDR_DRAW_SCAN` marker, the focused 128x128,
6-face, 8-mip FP16 cube also retains its mirror bytes immediately after mirror
creation, before installing that texture's traps. The bytes count against the
existing 32 MiB live CPU payload budget (at most 4 MiB per creation snapshot).
The existing input proof writes `*_provenance_creation_guest.raw` and reports
creation/upload guest CRCs, byte equality and the first differing byte. These
are sequential CPU observations; concurrent guest writes are not excluded.
Creation means creation of this texture's mirror, not allocation of guest memory
or the first write to that physical region. A missing creation proof is not
proof that the original bytes were valid.

The first focused TIC observed for that backing records its raw 32 bytes,
index, GPU virtual address, address-space identity, backing generation and GPU
write claim. A pre-capture ring retains the latest 4096 DMA/I2M commands; only
up to 128 most recent commands whose destination envelopes overlap that TIC's
range in the same address space are included in its origin metadata. The
report explicitly counts overwritten history and omitted matches. It snapshots
history when the first TIC is associated with the backing, not at GPU completion.

`raw_words` are hexadecimal, in these orders:

- DMA: launch, line length, line count, source pitch, destination pitch, remap,
  constant A, constant B, six source-surface words, six destination-surface words.
- I2M: the first 13 words of `Inline2MemoryBackend::RegisterState`, ending with
  launch configuration, excluding `loadInlineData`. The inline payload is not retained.

The destination envelope includes pitch gaps or the declared tiled allocation;
it is a candidate overlap, not an exact written-byte set. Address-space matches
are virtual: physical aliases, intervening GMMU remaps, direct CPU writes,
other engines, failed/skipped operations and command completion remain unproven.
No matching event does **not** rule out earlier writes. No extra guest-memory
read through trapped mappings, GPU copy, readback, explicit submission or wait is
added. The ring stores register metadata only and allocates lazily while the
startup marker is enabled. When enabled it adds bounded CPU bookkeeping, a
mutex per traced DMA/I2M command and creation-byte copies; FPS is unmeasured.

Install the updated **DevRelease** APK and fully restart with the marker present
before starting the game. Arm `ARM_CAPTURE` in gameplay as before and send the
new capture folder plus its matching emulation log. No new marker is required.
