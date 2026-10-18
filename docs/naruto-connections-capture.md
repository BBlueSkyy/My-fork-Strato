# Naruto Connections — four-frame mini-RenderDoc (diagnostic Draft)

This branch ports *only* the instrumentation from closed PR #357 onto master
after PR #359. It does NOT bring back the older #356 synchronization changes
and does not change production GPU rendering semantics when the capture is off.

## Capture both drivers on Moto G100

1. Install this PR's **reldebug** APK, not the old APK from #336/#357.
2. On Naruto Connections character selection, reproduce the P1 skills list
   flickering using Qualcomm proprietary.
3. In the public Strato files, open `gpu_capture/naruto_connections/` and rename
   `ARM_CAPTURE.rename_this_file_when_ready` to `ARM_CAPTURE`.
4. Return to character selection for four presentations. Wait for
   `MINIRD_CONNECTIONS wrote snapshot` in Info logs *after*
   `capture_complete.txt` before zipping the `capture_###` directory.
5. Upload full ZIP + matching `emulation.log` + screenshot/video.
6. Repeat with **Turnip 24.0.99**, same characters/menu, fresh session.

**Known symptoms:** proprietary driver loses the P1 skill bars transiently;
Turnip draws stable skill bars, but places the Connections logo on P1/CPU
character nameplates. The proprietary log had 29 `TIC_UNMAPPED`-equivalent
warnings for IOVA `0x588A90000` (these were not proven causal).
`Mirror Clamp To Edge` warning exists on both drivers. Proprietary enables
`needsIndividualTextureBindingWrites`, Turnip doesn't. One driver quirk
alone does not prove cause.

## Diagnostic budget and limitations

Four raw presented source frames, up to 24 globally named pass/atlas snapshots
(6/interval, 2 sampled atlases/interval), max 8 MiB per backing and 160 MiB
intermediate total; 12 MiB/presentation and 6000 bounded GPU events.
Supports compressed and uncompressed color atlases, full-sized targets,
actual descriptor-array counts and trait flags, shaders, CPU-side CBUF prefixes,
vertex/index data, TIC/TSC mapping, and Fermi2D/DMA events.

Presentation intervals are **approximate**, since rendering and presentation
are asynchronous. Renderpass splitting and readbacks may perturb timing
and temporarily mask a race. The first matching draw is not necessarily the
defective UI draw; compare log draw identities and pixels before inferring
cause. CBUF prefix dumps can be stale if host GPU writes were not read back.

Only **reldebug** is built on this diagnostic PR; normal master build outputs
remain unchanged. Do not merge this instrumentation. No game/driver hacks.

## Focused late-draw revision

Initial A/B dumps confirmed a Qualcomm P1 skill-list omission in one GPU
presentation source frame (not exclusive to either reused guest backing),
and a persistent Turnip Connections logo on both character nameplates.
The original capture filled its event quota in 1-2 approximate frames and
spent pass snapshots on the first draws of the earlier background.

This revision queues *no implicit background snapshots*. It explicitly
captures the primary full-size color target after qualifying UI draws
**96, 192, 384 and 768**, plus a single immediately preceding RT snapshot
and one sampled atlas at ordinal 96. Up to six snapshots per approximate
presentation interval; all GPU render-pass splits remain opt-in, within
160 MiB across at most 24 readbacks. Drawing is never skipped or changed.

Routine DRAW records are sampled 1/16 while events such as shaders, sampled
atlases at probes, Fermi2D, GMMU/TIC, descriptors, and presented sources
are retained, capped at 6000 total. Draw ordinals and interval indices are
not identical between asynchronous GPU drivers. These wide-spaced checkpoints
improve late-pass coverage but do **not guarantee** the precise bad UI draw.

## Dedicated Qualcomm text-chain capture (subsequent iteration)

The Oct 18 video and Qualcomm-only mini-RenderDoc prove *both* the logo
covering names and the missing P1 move list occur on the proprietary driver.
The previous text-like draws wrote to **1184×64 RGBA8**, using a **1024×1024 R8**
source with **456 indices**, or composited **1184×32 RGBA8** to **1184×64**.
The old capture only accepted full-size RTs, and consequently missed all
these tiny writer passes. This version captures the **first two glyph-to-strip
writes** (plus true pre-draw and first R8 atlas), the first strip-to-strip
composition, and the first text-strip-to-full-size consumer each approximate
presentation interval. These categories are selected by image geometry and
Vulkan format, not by game Title ID, shader hashes, or driver-specific
rendering behavior. Budget: at most six snapshots per interval, 24 globally,
160 MiB total. TEXT_FOCUS messages record pipeline hashes, writer/reader
identities, guest mappings, dimensions, indexed draw argument, and GPU
submission/render-pass IDs for correlation. Captures and render-pass splits
remain completely inactive until manually armed; they can perturb timing.

For now, repeat **only the Qualcomm proprietary** capture and log. Turnip
can be compared later. This is diagnostic only, not a functional fix.

## Consumer-paired capture — Qualcomm 0.777.0 (next test)

The first focused capture (3bf63d5) validated the R8 glyph atlas and the
1184x32/64 RGBA strips. It did **not** isolate a bad draw that consumes those
intermediates, nor establish why a BC3 logo could appear on the nameplates.
No functional GPU code has been changed. This diagnostic replaces the earlier
glyph and strip readback selection with two **full-target consumer** categories:

- `text_strip`: a blended full-sized color draw actually bound to an
  1184x32/64 RGBA text strip (geometry-based, no title/shader ID).
- `compressed_sprite`: a blended full-sized color draw sampling a 1024x1024
  compressed color asset, observed in the menu's full-resolution draws.

Each approximate presentation interval reserves at most **three ordered readbacks
per category**: destination *before* the selected draw, actual bound source
*before sampling*, and destination *after* the draw. The snapshots are ordered
in the same executor command stream, and grouped by `draw_id` and `role` in
`pre_*.txt` / `pass_*.txt` and `gpu_events.txt`. The source is deliberately
copied **before** the draw: source content captured afterward can be misleading
for aliased or overwritten textures. Two categories x three readbacks = the
existing budget of six snapshots per interval (24 overall, 160 MiB). If a
candidate's source is not eligible for staging or a budget is exhausted, the
file may be absent; check `PHASE2_SNAPSHOT` rather than assuming presence.

`TEXT_CONSUMER_DRAW` also logs **every qualifying candidate** regardless of
whether its snapshots fit in the quota. The line contains draw ID, shader
hashes, indexed argument, real sampled-image descriptor slots, image/mapping
identities, view mip/layer counts, executor/submission/renderpass IDs, and
current texture cycles. A cycle identity is not proof that it was the last
completed Vulkan GPU writer. `draw_id` correlates events and copies from a
single command stream; it is **not** a GPU-global execution order or an exact
presentation-frame identifier. `present_index_approx` remains approximate.

**How to collect:** install a new #360 reldebug APK if its build completes,
keep Qualcomm 0.777.0 selected, remain on the Connections character-selection
screen with the P1 skills intermittently missing, and arm a fresh capture as
above. Capture both a good and a bad P1 state if possible; send ZIP,
matching emulation.log and a screenshot/video. Do not capture Turnip yet.
Before inferring a cause, compare the paired pre/source/post files for each
`draw_id`, then compare ROI differences in P1 skills and both nameplates.
Because selection is bounded to the **first** candidate of each category in
each interval, this is not guaranteed to hit the bad nameplate; in that case
use the unfiltered `TEXT_CONSUMER_DRAW` lines to choose later ordinals for a
further focused run, rather than applying speculative sampler/alias changes.

This branch remains **diagnostic-only**, Draft and unmerged; it does not
revert or touch PR #359's runtime corrections. Capture readbacks are opt-in and
can affect GPU timing. Do not monitor GitHub Actions automatically.

Each selected pre-draw diagnostic now writes indices/VBOs as
`ui_draw_i<interval>_d<draw_id>_<name>.bin`, constant buffers as
`ui_i<interval>_d<draw_id>_s<stage>_cb<index>.bin`, and SPIR-V as
`ui_i<interval>_d<draw_id>_shader_<hash>.spv`. The ID prevents the
text-strip and compressed-sprite captures from overwriting one another.
Inspect `UI_ACTUAL_CBUF` / `UI_ACTUAL_SAMPLER` / `UI_PRE_SHADER` for
corresponding IDs. These dumps are read-only diagnostics, not emulation fixes.

## Intermittent-flash capture extension (Qualcomm 0.777.0)

The user confirmed the P1 move list was **actually flickering** during the
previous recording, while the Connections logo over nameplates appears
**randomly**, not consistently. The preceding capture only recorded four
presentation sources between ~83.890 and ~84.011 seconds (about 121 ms);
it therefore cannot be interpreted as four seconds of glitch-free rendering.

For a single manually armed capture, this revision reads back **24**
consecutive pre-swapchain presentation source images (roughly a one-second
span, depending on actual frame pacing). Detailed pre/source/post draw
snapshots remain restricted to the original first four approximate intervals:
6 readbacks per interval, 24 total, 160 MiB total budget. Frames 4..23
have no extra detailed draw readbacks; the approximate interval marker is
clamped to 4 (out of eligibility). The existing static diagnostic image
readback may affect frame pacing; do not treat timestamps as a normal
performance trace.

This gives a longer source sequence to check whether corruption is present
**before** the Android swapchain copy, but does not capture pixels *after*
the swapchain copy. For an exact on-screen comparison, provide a **screen
recording** made simultaneously with capture (preferably including a few
seconds before and after the one-second capture). If video shows a glitch but
none of the 24 source images does, that is evidence to investigate
presentation scheduling, timing perturbation, or post-source stages, **not**
proof that the Vulkan swapchain itself is defective.

Keep Qualcomm proprietary as first priority. No gameplay rendering changes,
title-ID hacks, new performance waits outside the opt-in probe, merge, or
automated GitHub Actions monitoring.
