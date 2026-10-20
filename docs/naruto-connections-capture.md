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

## P1 blinking reproduced — next Fermi2D boundary probe

On Qualcomm 0.777.0, 24 pre-swapchain presentation source images recorded
between log times 92.870 and 94.242 seconds (1.37 s) show complete P1 move
list disappearances at frames **15** and **18**; the CPU list remains visible.
The list returns at frames 16 and 19. An offline-only crop at x=100..509,
y=475..561 shows near-black-pixel fraction dropping from ~0.756 to
0.229/0.225 in these two failures, while CPU stays around ~0.77–0.78.

Around these events, the trace consistently includes two full-screen flipped
Fermi2D blits and approximately 134 texture-consumer draws per interval.
However, presented frames and draw events are asynchronous; neither proves
those draws produced the right pixels. The logo-over-nameplate artifact does
not clearly occur in this 24-frame series.

**New manually armed probe:** read back the actual source image immediately
before a full-surface inverted Fermi2D blit in that same executor command
stream. The first observed source/destination texture pair defines the
direction for the capture, and the reverse blit is skipped. Independent
budget: at most **24 snapshots** and **72 MiB**, in addition to the
existing 24 detailed draw readbacks and 24 VI presentation sources.
No fixed title, shader hash, guest IOVA, frame number or game-specific
runtime screen coordinate is used.

The files are fermi_input_XX_<width>x<height>_<format>.raw, with matching
fermi_input_XX.txt metadata; FERMI_INPUT_SNAPSHOT events log executor,
submission, source/destination identities and guest mappings. Copies are
GPU ordered strictly *before* the blit, with no CPU wait or device idle
outside the preexisting opt-in presentation capture. To compare the
pre-blit image and the presented source, account for vertical inversion
and align by content and temporal event order; the ordinal Fermi ID
is **not** a synchronized presentation frame_id.

If a Fermi input already lacks P1, focus on upstream Maxwell3D/texture
reads. If it contains the P1 list but the corresponding presentation
source lacks it, focus on Fermi2D, synchronization and presentation.
Neither alone identifies the exact cause. No runtime functional fix.
Keep PR #360 Draft and preserve #359, without monitoring Actions.

## Corrected Fermi source selection (2026-10-18, next manual capture)

The latest 24-frame Qualcomm capture includes all 24 new Fermi input
snapshots (1120x630 RGBA8), but each shows **background and characters
only**, before any skill-bar or nameplate UI is composed. Absence of P1
bars in these snapshots is expected; it is **not evidence** for a broken
Maxwell compositor or Fermi2D blit.

The live trace establishes the actual producer/consumer chain by *host
texture identity and guest mapping*:

1. Forward Fermi2D: source texture 0xB4000079EFA95668 (guest
   0x18BF6B0000) -> target 0xB4000079EFA975A8 (guest 0x180FCA4000).
2. 3,206 sampled-image UI consumer draw records target precisely
   0xB4000079EFA975A8, which is the Fermi forward destination.
3. Reverse Fermi2D: source 0xB4000079EFA975A8 -> target
   0xB4000079EFA95668. The **reverse source**, not the forward source,
   contains the composed UI that needs to be checked for flicker.
4. The VI presentation sources are further texture backings
   (not necessarily the same as the reverse Fermi target), so the path
   must not be conflated with direct scan-out of the intermediate.

The above addresses are **offline observations only**, not embedded in code.
The capture now tracks real full-resolution targets of qualifying
Maxwell3D UI consumer draws, and selects Fermi blit inputs **only when
that same live texture was observed as a compositor target**. It then
locks the matching source/destination direction for its usual quota:
24 snapshots/72 MiB. The unchanged source filenames remain
fermi_input_XX_*; metadata now labels the post-compositor / pre-reverse
boundary, and events use FERMI_COMPOSITED_UI_SOURCE. The raw image may
be upside down versus VI; validate orientation visually when comparing
by content, not ordinal alone.

This is still *diagnostic only*, not a functional GPU fix. #359 remains
intact; no title-ID/driver/guest-address selector was introduced.

## P1 omission confirmed before reverse Fermi — intermediate diagnostic

On Qualcomm 0.777.0, the corrected 24-frame capture contains **23** fully
composed Fermi inputs and **24** VI sources. After flipping Fermi vertically,
every source N most closely matches VI frame N+1 by full-image MAE. In both
failed instances, Fermi inputs **14** and **19** already lack the P1 skill
list, and VI frames **15** and **20** repeat the omission. Neighboring inputs
retain the bars. The CPU list remains present. The P1 ROI near-black
fraction decreases from ~0.75–0.77 in working sources to 0.292 (Fermi14)
and 0.276 (Fermi19). Image coordinates were used only for offline analysis.

The 134 qualifying texture consumer records per group are **identical in
ordered signature** between working frame 13 and failed frame 14, including
sampled source identities, shader hashes, and target identity. So there is
no observed missing qualifying GPU command; a draw can still have been
executed without contributing pixels, and routine DRAW sampling excludes
some other command classes. No basis exists to modify Fermi2D or Vulkan
presentation; those stages follow the already incomplete source.

**New opt-in probe:** take one readback of the live full-size Maxwell
compositor target just before its *first* qualifying text-strip draw.
The first qualifying strip triggers the readback; later draws in that
cycle do not repeat it. A reverse Fermi input event resets the
counter so the next UI composition cycle can be sampled. These are generic
runtime observations, not hardcoded title identifiers, shader hashes,
guest addresses or P1 screen coordinates.

Files: ui_pretext_XX_<width>x<height>_<format>.raw plus matching .txt;
trace marker UI_COMPOSITOR_PRETEXT. Maximum **24** midpoint images and
**72 MiB**, independent of prior Fermi/VI/draw quotas. GPU copying and
memory barrier are in the same command stream before the first strip;
normal emulation remains unchanged outside the manually armed capture.

**Next:** align each midpoint by content/temporal order (ordinal is not exact
VI frame ID) with the already captured reverse Fermi source. If the P1
list appears at the midpoint but is absent pre-Fermi, investigate later
Maxwell passes/overwrites. If absent at midpoint too, investigate the
earlier Maxwell drawing/texture/state/synchronization. This does not itself
prove a particular synchronization/barrier bug. PR #359 preserved, #360
Draft and unmerged; no Actions monitoring.

## New capture: failed midpoint trigger, frame 6 P1 disappearance

The latest Qualcomm 0.777.0 capture has 24 VI images at 1600x900,
13 pre-reverse Fermi sources and 24 focused readbacks. The intended
midpoint probe produced **zero** files and no UI_COMPOSITOR_MIDPOINT
events. This is a diagnostic selection error, not a missing guest draw:
the trace records **exactly one** qualifying full-resolution text-strip
consumer per composition cycle, whereas the previous probe required two.
That draw follows about 131 compressed-sprite consumers.

Fermi input 05 lacks P1 bars and corresponds to VI 06, where the same
bars remain missing, while CPU stays normal. Offline fraction of near-black
RGB pixels in the P1 bar crop was 0.5518 (Fermi 04 normal), 0.0805 (Fermi
05 failed), 0.0826 (VI 06 failed), and 0.5469 (Fermi 06 recovered). CPU
fraction stays ~0.656. All 13 pre-reverse inputs most closely match
presentation source N+1 by full-frame comparison after vertical inversion.

The corrected *diagnostic-only* probe runs **before the first text-strip
consumer** of a given full-resolution RT per composition cycle, after the
earlier sprite draws. It rearms at the pre-reverse Fermi boundary.
New file prefix: ui_pretext_XX, trace event UI_COMPOSITOR_PRETEXT.
Budget remains 24 snapshots /72 MiB; at 1600x900 it allows only 13
full-sized images, so collection can stop before all 24 presentation frames.
No game ID, shader hash, IOVA or screen-space region is embedded.

Next capture must compare pre-text output and pre-reverse Fermi source for
a P1 failure. If P1 bars are absent pre-text, the failure is in the
earlier sprite composition or input state; if present pre-text but absent
pre-Fermi, the text draw or subsequent writers require examination.
The logo-over-nameplate glitch was not clearly captured. The production
GPU renderer remains unchanged, PR #359 preserved, #360 Draft/unmerged,
and no GitHub Actions monitoring.

## Two text-strip draws per Maxwell submission — updated checkpoint (2026-10-18)

The diagnostic APK at commit 065d611 yielded a complete capture: 23
ui_pretext snapshots, 23 composed reverse-Fermi sources, 24 VI sources,
and 24 focused draw screenshots at 1120x630 on Qualcomm 0.777.0.

**Pixel proof:** In nearly every ui_pretext image the P1 move list is
not yet present; its absence before the first text draw is NORMAL.
Normal Fermi output has three bars, some intervals only two, and the
output of cycle 10 has **no** P1 move-list pixels added at all.
In an offline crop x=65..515, y=436..569, the fraction of dark RGB pixels
(<60 each channel) for cycle 10 is **0.023 before text**, **0.024 before
the reverse Fermi blit**, and **0.023 in VI frame 11**. The regional
mean absolute channel difference between pre-text and composed Fermi is
only ~0.05/255. For a normal cycle 8, dark fractions are
0.021 before text and 0.586 after; regional MAE is ~91.99/255.
The CPU dark-pixel fraction increases from ~0.216 to ~0.586 in the same
bad cycle, so the compositor still updates the other side. Intermediate
cycles 7, 9 and 11–14 produce only part of the P1 list.

**Corrected draw count:** examining the *entire* gpu_events trace between
pre-text and Fermi reveals **TWO** text-strip consumer draws per ordinary
Maxwell submission, separated by ~60 sprite draws, not one. The earlier
statement that only one was emitted was based on the selection behavior
of the previous diagnostic, and was incomplete. The corresponding two
draws in normal cycle 9 and failed cycle 10 have identical shader hashes,
live source image identities and destination; the only difference in
the qualifying post-pretext draw signature list is an extra unrelated
54-index sprite draw in cycle 10. This does not prove source pixels,
shader execution or blend state match. Both text-strip draws are
scheduled in the same Maxwell submission (e.g. 2363 versus 2365),
which is more reliable for grouping than present_index_approx.

**New diagnostic-only probe (after 065d611):** stop allocating the
redundant full-size pre-first-text readbacks and spend the same bounded
budget (24 images, 72 MiB) on the **actual render-target contents
immediately AFTER the second text-strip consumer in each Maxwell
submission**. Group by executor identity, Maxwell submission number and
target backing, incrementing at the point where those actual draws are
recorded. This does not use the approximate VI interval to count draws.
The files are ui_posttext2_XX_* with corresponding .txt metadata and
UI_COMPOSITOR_POSTTEXT2 trace events. The original Fermi source and
VI source snapshots remain enabled and capped. It can perturb a race
while a manual capture is armed but adds no waits or rendering changes
during normal emulation.

**Next decision:** align each ui_posttext2 with pre-reverse Fermi and
VI by observed content/submission. If a bad P1 is already missing
immediately after the second text draw, investigate the text producer,
sampled RGBA strip, blend and preceding sprite work. If present there
but absent pre-Fermi, investigate the later sprite draws or resource
overwrites in Maxwell3D. Either case must be verified before a
functional change. The intermittent logo-over-nameplate artifact is
not clearly visible in this capture. Preserve #359; PR #360 remains
diagnostic-only Draft and unmerged; do not monitor Actions.


## 1600×900 Qualcomm capture: text-strip classifier corrected (2026-10-18)

The latest capture has 24 complete VI sources, 13 pre-reverse Fermi
sources (72 MiB budget exhausted at 1600×900), 12 initial draw
snapshots, and 3,066 TEXT_CONSUMER_DRAW records. **All 3,066 records
are kind=compressed_sprite**; there are **zero** kind=text_strip,
zero ui_posttext2 files, and zero UI_COMPOSITOR_POSTTEXT2 events.
The lack of this probe is a *diagnostic selection failure*, not
proof that guest text draws disappeared.

The routine DRAW samples (1/16 only) reveal actual full-screen
blended consumers bound to **1664×96, 1664×64, 768×96, and 704×96
RGBA8** source images. Our IsDiagnosticTextStrip filter previously
accepted only width 1024..1280, inherited from 1120×630 captures.
Thus it incorrectly excluded these real 1600×900 compositor inputs.

**Pixel result:** VI frame 16 loses almost all P1 bars and frame
17 restores them. Offline dark-pixel fraction RGB<75 in
x=95..624,y=655..814: VI15=0.5487 (partial), VI16=0.0745 (absent),
VI17=0.6688 (full). The CPU equivalent remains approximately 0.668.
VI frames 9,11,13,15,21,23 show varying partial lists.
The reverse-Fermi buffer aligns to VI N+1 for the 13 pairs available,
but the strongest VI16 failure is OUTSIDE the saved Fermi budget.
The Connections logo error is not clearly shown in this session.

**Diagnostic-only updates after 951a0c0**:
- Use a reusable shape predicate width 512..2048, height 16..192,
  width>=6×height, retaining the RGBA8 format, color-image checks
  and staging compatibility. Tests cover original 1184×32/64 and
  observed 1664×64/96, 768×96, 704×96 sizes, plus negatives.
- Count text consumers **per compositor target between reverse
  Fermi blits**, not per individual Maxwell submission. The same
  UI composition can span multiple executor submissions. The
  second text draw should produce ui_posttext2_XX images and events;
  this grouping does not imply exact present-frame association.
- Increase only the manually armed post-text readback budget from
  72 to **144 MiB** to cover 24 × 1600×900 RGBA8 frames (138.24 MB).
  Retain a hard cap of 24 snapshots. Normal emulation does no extra
  readback, CPU waits or special rendering.
- Preserve #359 and all production paths. This is not an emulator
  bug fix until a source/draw/attachment mismatch is demonstrated.

**Next Qualcomm test:** after a new #360 reldebug APK compiles,
check that the ZIP contains ui_posttext2_XX.raw/.txt. If it does,
compare each post-text target with pre-reverse-Fermi input and VI,
using metadata + image-content alignment. If absent again, inspect
format/transfer capabilities and actual binding traces without
inferring a rendering defect from diagnostic absence.


## Post-second-text capture verified; late-P1 probe moved to fourth text draw

On the latest 1120x630 Qualcomm 0.777.0 capture, the diagnostic produces
**23 complete ui_posttext2 images**, 23 reverse-Fermi input images, 24 VI
presentation sources, and 24 focused first-four-interval pass snapshots.
The trace contains **3,210** qualifying UI consumer events: 3,069
compressed sprite consumers and 141 text-strip consumers. No output file
write failures were observed.

**Finding:** all 23 ui_posttext2 images already contain two CPU move-list
bars but **do not yet contain P1 move-list bars**. This is expected:
every full UI composition cycle contains approximately **six (sometimes
seven) text-strip draws**, and the post-second-text boundary occurs
early, before a large block of sprite draws and the third/fourth/fifth
text draws. The UI P1 list appears between this boundary and the
composed reverse-Fermi input, so another pre-Fermi-copy/wait fix is not
warranted.

At reverse-Fermi input **20** the P1 move-list crop is significantly
reduced (fraction of pixels RGB<80: ~0.4945 versus 0.7154 at input 19
and 0.7270 at input 21). The CPU side remains ~0.754. A character-color
region overlaps the P1 list in this particular source, but its relationship
to the reported flicker is not yet causally proven; animation or a late
sprite draw may be responsible. The logo/nameplate glitch is not
conclusively reproduced here. Across all 23 pairs, Fermi source N best
corresponds to VI source N+1 after vertical inversion.

**Opt-in diagnostic change:** use the exact same independent 24-snapshot,
144 MiB budget for **ui_posttext4** instead of redundant ui_posttext2.
Track text-strip draws by live compositor target until the reverse
Fermi2D copy, with explicit four-stage counter: first/second/third
increment; fourth queues ordered render-target readback; fifth/sixth
cannot allocate again. Reset counter on reverse-Fermi boundary and new
manual sessions. New metadata files named ui_posttext4_XX.txt, raw
ui_posttext4_XX_* and trace event UI_COMPOSITOR_POSTTEXT4.
This checkpoint follows the first large group of intervening sprite
draws; it may or may not already include P1 bars and must be verified
by the next capture.

This change does not alter production rendering, shader, sampler,
texture matching, memory aliases, or synchronization. No title-ID,
fixed shader, fixed IOVA, or ROI is used to select the probe. #359
preserved; #360 Draft; do not merge or follow Actions automatically.
