# Naruto UI phase 4 — small glitches after PR #356

## Confirmed result

The user tested PR #356 and confirmed it eliminated the full UI flicker.
The combined diagnostic PR #357 preserved that fix. Its Naruto Storm 4
capture showed that the black rectangles and fragmented heading already exist
in the UI render target **before** the Fermi2D vertical flip; the Fermi2D
copy carries the corruption forward rather than introducing it.

The source of the corruption is **not yet proven**. Potential contributors
include BC3 atlas contents/layout/mips, texture alias freshness, blending,
texture coordinates and per-instance vertex state. No shader-hash, game-ID or
driver-specific functional workarounds have been added.

## Phase 4 capture strategy

When manually armed only, the capture now observes:
- **UI sprite draw boundaries 1, 3 and 6** per *approximate* presentation
  interval. A render-pass boundary is inserted after these selected draws,
  allowing an intermediate image snapshot after each. This changes
  render-pass grouping during diagnostics; it is NEVER enabled in normal play.
- Up to **three unique compressed texture atlases per approximate interval**,
  deduplicated across the four intervals. Atlas files are named
  `atlas_XX_rpYY_WIDTHxHEIGHT_FORMAT.raw`; accompanying `atlas_XX.txt`
  contains dimensions, Vulkan block-compression format, image mip count,
  guest mapping, executor and submission. Raw BC3 blocks need offline
  decompression, including handling any mip-level packing.
- Full-screen UI outputs and their downstream consumers as
  `pass_XX_rpYY_WIDTHxHEIGHT_FORMAT.raw` and `pass_XX.txt`.
- Fermi2D source/destination blits, MaxwellDMA commands, presented frame
  sources, backing addresses and completion-cycle identities in
  `gpu_events.txt`.

Maximum intermediate readbacks: 8/approximate interval, 32/session, still
bounded by **160 MiB** across all intermediates. The image and event counts
may be lower than those limits. Four actual presentation sources are saved
separately. Since the presentation counter is asynchronous, an interval does
**not** establish exact correspondence to a presented frame.

## Android procedure

1. Install **only the new PR #357 reldebug APK** after its build succeeds.
   The older #357 APK captured final render passes but lacked per-draw/atlas
   snapshots. A full release APK is deliberately skipped for diagnostic
   branches to reduce compilation time.
2. With Naruto Storm 4 displaying the broken `Game Mode Select` menu,
   find the public Strato directory `gpu_capture/naruto_ui/`.
3. Rename `ARM_CAPTURE.rename_this_file_when_ready` to `ARM_CAPTURE`.
4. Keep the menu visible until four presented frames have been captured.
   Wait for all `MINIRD_NARUTO_PHASE2 wrote snapshot` messages; the presence
   of `capture_complete.txt` is **not** sufficient to prove all async image
   callbacks have written files.
5. Zip the newly created `capture_###` directory and include the matching
   `emulation.log` and one screenshot showing the remaining artifacts.

Check for the events `UI_DRAW_PROBE`, `ATLAS_SAMPLE`, `PHASE2_SNAPSHOT`
and filenames starting with `atlas_`. The first corrupted *draw-stage*
snapshot narrows the cause further. A correct atlas image with a broken
first draw shifts attention to UV/vertex/blend/shader semantics; corrupted
source atlas data shifts attention to texture layout, cache and upload.

No diagnosis is complete until the extracted raw images have been inspected.

## Validation and CI

Host test: `bash tests/naruto_ui_capture/run.sh`.
The PR workflow for mini-RenderDoc branches builds **only reldebug**
(`assembleDevReldebug`), with Gradle build cache. All regular PRs continue
building release and reldebug. Existing GPU host regressions and the new
capture-budget test still run. CI green is not evidence that a graphic fix
was achieved.
