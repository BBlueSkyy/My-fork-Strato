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

## Phase 5.1: select the correct sprite compositor (Oct 18 capture correction)

The first Phase-5 on-device capture selected a **scene** renderer with 92
indices / 1 instance, texture 1024×512 BC3, output guest map 0x841694000.
The four `pre_XX` images showed the sky, not the menu overlay: the first
pre/post pair changed only 1,279 of 705,600 pixels (0.18%). Meanwhile
the targeted UI was rendered in a **different executor** after a Fermi2D
full-surface vertical flip, at guest mapping 0x1855724000, with a 30-index,
2-instance first draw. That capture cannot prove when the black shards
appeared in the true UI render target.

The probe now discovers the destination of the **most recent full-surface
negative-height Fermi2D blit within the current presentation interval**.
It only claims a blended, compressed-source sprite draw when its color
attachment physically overlaps that discovered destination. No title ID,
texture pointer, guest address or shader hash is hardcoded. The active
target is tracked with `FLIPPED_UI_DESTINATION` events, and the relevant
`PRE_UI_DRAW` + `UI_PRE_DRAW_STATE` must show the **same guest backing**
as the Fermi destination. In this title, the expected *draw-shape* is
30 indices and two instances (based on the preceding evidence).

For capture acceptance, verify:
- `FLIPPED_UI_DESTINATION` appears before `PRE_UI_DRAW` in each interval
  and both map to the **same guest bytes**.
- `pre_XX` shows the vertically inverted UI background/overlay,
  **not** the ordinary upright sky-only scene.
- The immediate post-draw `pass_XX` captures that same color target.
- If any prerequisite fails, do not interpret the pre/post pixel delta
  as the first UI draw's output.

The #356 flicker correction remains intact, and snapshots are still
bounded to 160 MiB. Only this capture's target selection changed.

## Phase 5: exact pre/post comparison and draw-state capture

This new diagnostic is designed around the remaining **black blocks in the
2D UI**, not the previously fixed whole-UI flicker.

A new `pre_XX_rpYY_WIDTHxHEIGHT_FORMAT.raw` (and `pre_XX.txt`)
now reads the **color destination before the first blended draw sampling
a compressed atlas** in each approximate presentation interval. The
readback is placed in the same GPU command stream **before** the draw is
recorded. The corresponding `pass_XX` from the first sprite-draw boundary
provides an after-draw comparison. The two images must be matched by
`texture`, `guest_map`, `executor` and `present_index_approx`; IDs are not
stable across separate boots.

The events include `PRE_UI_DRAW`, `UI_PRE_DRAW_STATE`,
`UI_PRE_DRAW_BLEND`, `UI_PRE_VERTEX_BINDING`,
`UI_PRE_VERTEX_ATTRIBUTE`, `UI_PRE_SAMPLED`, `UI_PRE_SHADER`
and `PHASE2_SNAPSHOT ... kind=pre_ui`.

The draw-state records contain indexed/draw offsets and instance count,
all enabled vertex stream strides/rates/divisors, active attributes' stream,
offset, numeric type and component width, color blend factors and ops,
and sampled atlas view format/base mip/mip count/layer range.
These details let us directly check guest vertex/instance interpretation and
blending against Eden/Ryujinx instead of issuing another broad GPU rewrite.

If the `pre_XX` already has the black shards, the first sprite draw is
**not proven to create them**: trace the preceding producer. If `pre_XX`
is clean but the corresponding post-draw `pass_XX` has shards, inspect
the captured vertex/UV/blend state and exact source atlas.

The probe itself inserts a render-pass boundary and image readback only
during the armed capture, so it can perturb frame timing. Normal gameplay
and the #356 submission-order fix are unchanged. No Title ID, hash-based
rendering hack, or sleep is used.

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
