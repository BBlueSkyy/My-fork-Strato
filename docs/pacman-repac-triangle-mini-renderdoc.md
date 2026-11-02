# PAC-MAN WORLD Re-PAC — inverted/giant triangle diagnostic (stage 1)

Latest paired results and the callback-only submission-contract experiment:
[Stage 10](pacman-repac-stage10-submit-contract.md). Keep threshold 256 and
the PR Draft; no root-cause rendering correction has been established.

This diagnostic is **not a graphical fix** and not a full RenderDoc replayer. It
instruments the real Maxwell3D draw paths and the source passed to Android
presentation. It does not alter shaders, index buffers, topology, depth/cull
behavior, resource management, barriers or synchronization.

## Reproduction

The user supplied a 16.6 s gameplay recording (Moto G100 / Strato).
Around 3 s a large dark triangular region crosses the sky and a bright
white clipped region obscures palm trees and the beach. Around 9 s the
same scene has normal-looking sky and beach. This supports a transient GPU
rendering problem; **no particular draw call or source resource is proven**.

## Manual collection

1. Install the diagnostic ARM64 **DevRelease** APK built from this draft PR.
2. Run PAC-MAN WORLD Re-PAC until the beach/wooden-platform scene where the
   artifact reproduces. Keep the game running.
3. In the app's public files directory, create the empty marker
   `gpu_capture/pacman_world/ARM_CAPTURE`. The parent folder is created
   automatically by the presentation thread. On Android 12, use a file
   manager with access to the app's Android/data files or an authorized
   Android/ADB file workflow.
4. Keep the corrupted scene visible for at least six displayed frames.
   The marker is checked approximately every eight host presentations.
5. Find `gpu_capture/pacman_world/capture_###/gpu_events.txt` and
   `capture_complete.txt`; send their ZIP, `logs/emulation.log`, and
   a screenshot/video from the same scene.
6. Repeat on the same scene when **not** corrupted, with identical driver,
   version, Free Guest Texture Memory and readback settings. A/B compare
   draw IDs/order, shaders, culling, winding, topology, depth and targets.

No console commands or modifications to game ROMs are needed for the text
trace. An optional external Android RenderDoc Vulkan layer makes the
already-existing `captureNextExecution` path eligible to capture **one GPU
execution/submission**; the text trace does not require this layer.
No `.rdc` file is promised when the layer is absent.

## Recorded information

- Six host presentation intervals (approximate; not guest frame IDs).
- Presented source texture pointer, width/height, format, Android
  crop/transform and destination swapchain extent.
- Direct, indirect and inline-index Maxwell draws, guest topology and
  count/first/instance/offset, selected pipeline shader hashes,
  rasterizer discard, front-face winding, culling, flip Y,
  primitive restart, depth/stencil test and writes, blending and color mask.
- Scissor, color/depth attachment identity and dimensions, first eight
  sampled image views, full sampled view count.
- At most 12,000 lines of GPU events, with dropped count, and one
  optional RenderDoc-submission request per capture.

The one-shot session automatically stops after six presentations.
No per-frame readbacks, extra render-pass splits, device-wide waits or
diagnostic behavior are enabled outside the arm window.

## Interpretation and next implementation step

1. If both good and bad traces use identical draw state, this does not rule
   out invalid vertex/index data, shader outputs or memory aliasing.
   A second **GPU-ordered before/after draw image and buffer snapshot** must
   target the suspect shader/draw and attachment discovered here.
2. If the bad frame changes winding/cull/depth topology or scissor, trace
   the guest registers and fix the first incorrect conversion, with a
   regression test for the exact state.
3. If color output is already wrong at the scene RT, investigate shader
   execution and/or corrupt geometry. If the scene output is correct but
   presentation is wrong, investigate copy/layout/alias of the source.
   **This stage records metadata, not raw RT pixels, and cannot distinguish
   those last two cases by itself.**

Do not claim a correction or a validated fix before an on-device bad/good
capture and targeted regression test. Keep this PR draft, unmerged.

## Build status

Source integration only; no Android toolchain build or Moto G100 test was
performed when the source was authored. GitHub Actions results should be
checked separately.


## Device capture 001: observed data (2026-11-01)

Files: `capture_001.zip` (959,293 bytes of plain-text GPU events before ZIP)
and `emulation.log`. PAC-MAN WORLD Re-PAC v1.0.2, handheld,
Adreno 650, Qualcomm proprietary 0.777.0, Vulkan 1.1.276.

- Six presentations, 1,580 direct draws, 53 unique shader combinations.
  No dropped events. No indirect or inline draws during this window.
- Two full draw intervals have 395 draws each; a third has the same count.
  Intervals 3 and 4 split one sequence at the asynchronous present boundary
  (184 + 211); **these are not complete independent guest frames**.
- Per complete sequence: approximately 183 depth-only draws to a
  2048×2048 depth target, followed by a 1280×720 scene/color pass and
  postprocessing. The main color target uses Vulkan format 122,\n  `VK_FORMAT_B10G11R11_UFLOAT_PACK32` (packed HDR floating point).\n  This makes scene HDR data worth examining for anomalous pixels,\n  but no pixel values were captured in stage 1. Guest topologies 4\n  and 7, mostly indexed. All
  `flipY=false`; front-face mostly counterclockwise. No evidence of a
  global flip regression from this data.
- Three alternating 1920×1080 VI sources. Source format IDs alternate
  between 37 (R8G8B8A8 UNORM) and 43 (R8G8B8A8 sRGB), with a
  1280×720 crop. This is a potential format identity to compare, **not
  an established cause of the triangle**.
- The RenderDoc layer is absent. Text metadata alone cannot identify
  the first draw producing the bad pixel.
- The log contains an audio latency error unrelated to any verified
  graphics failure; it contains no identified Vulkan failure.

### Stage 2: scene image checkpoints (same Draft PR)

Create an empty `gpu_capture/pacman_world/ENABLE_SCENE_SNAPSHOTS`
marker **before** arming a new `ARM_CAPTURE` marker. Keep the game at the
affected beach scene. As in stage 1, `ARM_CAPTURE` is automatically consumed.

An opt-in readback now samples the actual 1280×720 scene color target
**after** direct scene draws 1, 16, 64 and 176, within approximate
presentation intervals 1 and 2. Only full-resolution 2D, single-layer,
single-mip, uncompressed color RTs with transfer-src and eligible Vulkan
layout are sampled. Maximum eight images, 8 MiB each and 64 MiB overall.
There is no fixed shader hash or game ID in the selection.

This diagnostics-only path ends a render pass for GPU-ordered readback,
which can change scheduling/timing while armed. It is **not a test of
performance or proof of semantically identical rendering under capture**.
Outside manual capture, the code does not touch render passes.

For stage 2, upload:
- The entire new `capture_###` ZIP, including `scene_*.raw` and
  `scene_*.txt`.
- Both `capture_complete.txt` and `snapshot_readbacks_done.txt`:
  the first only closes the metadata window; the latter confirms that
  GPU fence completion and asynchronous file writes have finished.
- The corresponding `emulation.log` and screenshot or video clearly
  showing whether the **captured** scene was bad or good.

Compare per-draw images and earliest divergence; refine the binary
search with new draw ordinals if necessary. No rendering fix should be
committed before evidence ties a specific GPU operation to the defect.

**Stage 2 build and phone execution have not been validated yet.**


## Stage 2 results and Stage 3 (November 1, 2026)

The uploaded second `capture_001.zip` successfully contains six ordered,
post-draw `B10G11R11UfloatPack32` images and a completed async readback
marker. It captures no after-draw-176 image: this title's sampled scene has
101 eligible full-resolution color draws per interval, so ordinal 176 was
unreachable.

The visible triangular discontinuity and overly bright region already
exist after draw 1; after draw 16 they persist; after draw 64 much of the
affected area has been replaced. HDR RGB values above 10 occupy
4.673% / 4.673% / 1.330% of the first interval's pixels, respectively.
The same pattern appears in interval 2. There are no NaN/Inf values.
The 11,264 peak is finite and **does not prove an HDR bug by itself**.

**Stage 3 has been published in the same Draft PR**. The normal
manual triggers are unchanged: create
`gpu_capture/pacman_world/ENABLE_SCENE_SNAPSHOTS` before a new
`gpu_capture/pacman_world/ARM_CAPTURE`. Stage 3 now acquires a
**pre-first-draw** RT snapshot and post-draw snapshots at ordinals
1, 16, 32, 48, 64, 80 and 100, all within one scene interval. It
uses the existing eight-image/64-MiB quota and async readback completion
marker; no diagnostics run unarmed. These probes are intended to separate
old RT contents from the actual draw responsible for a new anomaly.

Send the newly generated `capture_###` ZIP with
`scene_*.raw`, `scene_*.txt`, `gpu_events.txt`,
`capture_complete.txt`, and `snapshot_readbacks_done.txt`,
plus `emulation.log`. A matching phone screenshot/video showing the
visible defect is important to separate expected scene lighting and
raw-image orientation from actual display corruption.

Full numeric results and SHA256 fingerprints:
`docs/pacman-repac-capture-002-analysis.md`.

**Not a rendering fix.** Stage-3 Android compilation and capture
validation are pending. Preserve Draft, do not merge.


## Stage 4: automated opt-in HDR anomaly detection (after third capture)

The third capture produced eight successful 1280×720 GPU snapshots:
one before the first scene draw and seven through draw 100. The earliest
pre-draw image was **clean of extreme HDR values**, whereas the second
capture began with over ten thousand RGB pixels above 100. Only 298 raw
pixels changed between pre-draw and post-draw 1 in the third capture.

To scan across more rendered scenes without requiring exact timing, create
an empty `gpu_capture/pacman_world/ENABLE_HDR_SCAN` file and then
re-create `gpu_capture/pacman_world/ARM_CAPTURE`. The HDR scan marker
takes precedence over `ENABLE_SCENE_SNAPSHOTS` when both exist. It
samples after 1280×720 HDR scene color draw 16, which distinguishes the
previous bad and good runs. It can run for up to 60 host present intervals,
uses no more than 4 in-flight readbacks, and writes up to five RAW frames
(a baseline plus selected severe-HDR matches). Threshold: at least 512
pixels with any R/G/B HDR channel >100, verified against the previous
bad (~10,541) and good (0) samples.

After capture, upload `hdr_scan.csv`, `hdr_scan_done.txt`,
`capture_complete.txt`, `hdr_scan_*.raw`, `hdr_scan_*.txt`,
`gpu_events.txt` and `emulation.log`. Wait for the done marker
before archiving the files. For another fixed-checkpoint capture,
remove `ENABLE_HDR_SCAN` and keep `ENABLE_SCENE_SNAPSHOTS`.

This scan does **not** automatically diagnose a black-only triangle:
it searches specifically for the extraordinary HDR signal present in
the previous bad run. It can affect timing; a clean scan cannot
conclusively eliminate synchronization, shaders or driver causes.
No rendering correction has been made.

Third capture metrics and instructions:
`docs/pacman-repac-capture-003-analysis.md`.
Build and phone execution of this stage remain unverified.


## Stage 5 — Executor Flush Threshold = 0 removes the triangle

**New on-device A/B from user:** `Executor Flush Threshold=0` removes
the visible inverted triangle but loses performance, while default
`256` shows the artifact. This is a crucial observation, not yet a
validated source-level fix.

### Relevant executor behavior

`CommandExecutor::AddSubpass()` flushes after enqueueing a draw when
`slot->nodes.size() > *state.settings->executorFlushThreshold`
and `!gotoNext`. With threshold 0, nearly every 3D draw causes
`Submit()`. `SubmitInternal()` finishes the render pass, calls
`RecordFullBarrier()`, binds/updates attached texture and buffer
dependencies, then `RotateRecordSlot()`. Thus threshold 0 changes
**render-pass boundaries, GPU barriers, host resource bookkeeping,
submission ordering and pacing simultaneously**. It does not by itself
prove a missing Vulkan barrier or shader/compiler defect.

### Two isolation experiments added to this Draft PR

Use **Executor Flush Threshold = 256** for both tests. At the affected
scene, put an empty `ARM_CAPTURE` marker alongside exactly one of:

1. `ENABLE_EXECUTOR_RP_SPLIT_TEST` — end the render pass after every
   `AddSubpass()`, without forcing a Vulkan submission. Separates
   pass lifetime/load-store/dependencies from actual submit frequency.
2. `ENABLE_EXECUTOR_BARRIER_TEST` — do the same per-draw split **plus**
   an all-commands Vulkan memory barrier within the command buffer,
   again without forcing submission. Separates missing intra-command
   memory visibility from submit/CPU scheduling effects.

Create the marker in `gpu_capture/pacman_world/` and then create
`ARM_CAPTURE`; re-arm separately for the next test. The experiment
ends after **60 host presentations** and produces
`gpu_events.txt` and `capture_complete.txt` with
`executor_experiment_mode=1` or `2` and the count of affected
subpass enqueue operations.

Use only one test marker at a time; remove any `ENABLE_HDR_SCAN`
marker to avoid confusion. In code the experiment takes precedence
over both the HDR scan and stage-3 image capture, and no raw images are
copied. Tests deliberately perturb renderpass boundaries and may
reduce performance; neither mode is intended as a permanent hack.

**Interpret carefully:**
- If split-only fixes: investigate renderpass load/store preservation,
  attachment dependencies and subpass coalescing, under threshold 256.
- If split-only fails but barrier fixes: investigate Vulkan memory
  dependency/access/stage masks and guest attachment transitions.
- If neither fixes but threshold 0 does: investigate differences
  introduced by `SubmitInternal()`, texture/buffer cycle dependencies,
  execution order, descriptors/resource lifetime and timing.
- If both fix: more targeted tests are required; an unnecessary global
  barrier is not a safe final solution.

Reproduce on-device with the **same driver, scene and settings**,
ideally several game reloads per configuration since earlier
captures differed between runs. Record whether the visible inverted
triangle persisted during each experiment; a text log alone cannot
determine that. If the experiment itself suppresses corruption due to
timing, report it as inconclusive.

No change to the default threshold, no permanent barrier, no game-
specific shader hash, no GPU feature disabled; PR stays Draft,
unmerged. The experimental APK and device behavior are unverified.


## Stage 5 results and Stage 6 selective scene-submit experiment

Two complete on-device runs confirmed that both previously published
controls **did not fix** the inverted triangle:
- Mode 1 (renderpass splitting without additional submissions):
  14,691 instrumented operations, triangle still visible.
- Mode 2 (renderpass splitting plus a full Vulkan barrier):
  15,120 instrumented operations, triangle still visible.

The result makes standalone renderpass splitting or the tested Vulkan
barrier insufficient. It does **not** prove a specific root cause.
The important difference still untested is the actual invocation of
`CommandExecutor::Submit()` (which finalizes renderpass execution,
flushes deferred callbacks, rotates command recording slots,
updates resource dependencies, and changes driver submission timing).

### Stage 6: selectively submit scene draws while keeping threshold=256

A third temporary diagnostic mode was added to the **same Draft PR**:
- Leave the in-app `Executor Flush Threshold` at **256**.
- Remove `ENABLE_EXECUTOR_RP_SPLIT_TEST`,
  `ENABLE_EXECUTOR_BARRIER_TEST` and `ENABLE_HDR_SCAN`.
- Create an empty
  `gpu_capture/pacman_world/ENABLE_EXECUTOR_SCENE_SUBMIT_TEST` file.
- Create a new `gpu_capture/pacman_world/ARM_CAPTURE` file, enter the
  same beach scene, and observe whether the inverted triangle remains.
- This experiment lasts up to **60 host presentation intervals**.
  The recorded mode in `capture_complete.txt` must be **3**,
  and `executor_experiment_draws` counts only the targeted scene
  submissions (rather than all draws).
- Archive the capture and `emulation.log`, and also report the
  visual state of the triangle *during the armed experiment*.

For mode 3, `AddSubpass()` submits after an eligible
**1280×720 color-target draw**, while all other draws retain the
standard batching behavior at 256. Unlike mode 1/2, this engages the
actual `SubmitInternal()` machinery. The dimensions are an observed
diagnostic filter, **not a permanent game-specific fix**. If a draw
would normally choose a next-subpass continuation (`gotoNext`),
the experiment honors the existing no-flush restriction.
No global waits or permanent changes to memory synchronization are made.

If mode 3 fixes the artifact, investigate the resource dependency
between the depth-only pass, the HDR scene target and subsequent
sampling, and narrow the submission frequency until the responsible
producer→consumer boundary is identified. If mode 3 also fails,
evaluate submission fences around depth and postprocess passes,
resource lifetime, CPU/GPU pacing, and shader/driver timing; do not
apply global `Threshold=0` as a final workaround.

Even if the graphics happen to improve with the temporary mode,
its frame pacing/FPS may drop. A **permanent patch is not warranted**
until a precise dependency, state transition or resource-lifetime
fault is identified, reproduced and regression-tested.

Stage 6 code is published but the new ARM64 APK and on-device result
are **not yet validated**. Keep PR Draft, no merge.


## Stage 6 capture analyzed; duration bug fixed (2026-11-01)

The first mode-3 attachment `capture_001.zip` confirms successful
activation, but is **NOT an exhaustive negative A/B result**:

- `executor_experiment_mode=3`, `recorded_presentations=60`,
  `executor_experiment_draws=6213`, `dropped_events=0`.
- The log arms mode 3 at `172955013` and closes the 60-present
  metadata window at `175611024` (roughly **2.66 seconds**).
- The user reports that the triangle remained visible upon returning
  to the game after creating the files. The old implementation
  incorrectly made `ExecutorExperimentMode()` return **0** whenever
  the capture window ended. It automatically deactivated mode 3
  immediately after the 60 presentations, even though its marker
  remained on disk. The same duration limitation affected earlier
  mode-1 and mode-2 A/B testing.
- Therefore, the user may have observed the artifact **after the
  2.66-second intervention had ceased**. The old data establish
  activation and 6,213 selective submits only, not necessarily
  a visual comparison over an entire playable sequence.

### Revised A/B behavior

The opt-in experiment mode now remains latched **until app process exit**
(or until a new explicit capture replaces the mode). Only the metadata
capture ends after 60 host presentations. No unarmed/default behavior
changes. `capture_complete.txt` now records
`executor_experiment_persists_after_capture=1` for tests 1/2/3.

To retest **mode 3** on a new APK:
1. Fully terminate the Strato app (not simply background it).
2. With in-app `Executor Flush Threshold=256`, leave only
   `ENABLE_EXECUTOR_SCENE_SUBMIT_TEST` (not the split/barrier/HDR
   markers) in `gpu_capture/pacman_world`.
3. Recreate the empty `ARM_CAPTURE` file **before** starting
   the game, then boot the same save and scene.
4. Play long enough to determine whether the triangle is present
   while mode 3 is active. The experiment persists even after the
   text ZIP's 60-frame `capture_complete.txt` exists, until the
   application exits. Extra targeted submissions may lower FPS.
5. Send the resulting ZIP/log and the on-screen visual result.

One test per process. Removing a mode file after arming does not
instantly disable that mode; close/restart Strato to restore normal
`256` rendering. Do not merge test-only submission changes into
the default pipeline.

If mode 3 still fails after a complete A/B game run with the corrected
persistent test, treat selective *scene-only* submissions as
insufficient and prioritize the omitted depth, postprocess and
presentation producer→consumer boundaries. No root-cause fix yet.


## Stage 7 — identify which extra GPU submission matters (2026-11-01)

**Confirmed A/B:** The user reproduced a clean PAC-MAN scene (the inverted
triangle disappeared) with `Executor Flush Threshold=256` and the
persisting `ENABLE_EXECUTOR_SCENE_SUBMIT_TEST` flag. Its latest
`capture_001.zip` verifies:

- `executor_experiment_mode=3`;
- `executor_experiment_draws=6004` over 60 host presentations;
- `executor_experiment_persists_after_capture=1`;
- no dropped trace events; user additionally supplied an on-screen
  screenshot of the correctly rendered beach scene (~27 FPS).

The success is strong evidence that **extra command buffer submissions**
change the outcome. The previous renderpass-only and barrier+split
tests did not fix the corruption. Do not assume the precise bug is
a fence, a texture alias, the Vulkan driver or a shader without a
narrower controlled test.

**Correction of prior wording:** Mode 3 matched *any* 1280×720 color
attachment, not only HDR `B10G11R11_UFLOAT_PACK32`. It may thus submit
postprocess draws in addition to the primary HDR scene. Stage 7
splits these behaviors into smaller opt-in tests.

### Three new marker options

For each test, leave **Executor Flush Threshold = 256** in settings.
Fully terminate the Strato app between tests. In
`gpu_capture/pacman_world/`, leave only one experiment marker with
a freshly created empty `ARM_CAPTURE`. There must be NO other
`ENABLE_EXECUTOR_*` or `ENABLE_HDR_SCAN` markers.

1. `ENABLE_EXECUTOR_DEPTH_SCENE_TEST` — **mode 4**. After seeing
   a depth-only 2048×2048 draw, submit immediately **after the first
   following** 1280×720 **HDR** color draw. This is a diagnostic
   approximation to the depth→HDR transition. It does not submit
   *before* the HDR draw, because the current draw's allocator-backed
   state is already prepared when `AddSubpass()` executes.
2. `ENABLE_EXECUTOR_SCENE_POST_TEST` — **mode 5**. After seeing a
   1280×720 HDR color draw, submit after the **first non-HDR color
   draw** targeting a size other than the 1920×1080 VI-sized target.
   This approximates the HDR scene→postprocessing transition.
3. `ENABLE_EXECUTOR_VI_TARGET_TEST` — **mode 6**. After seeing the
   HDR scene, submit after the **first color draw to a 1920×1080
   image**, which matches the observed VI backing dimensions.
   This is **not necessarily** the actual `PresentFrame()` boundary.
   If no such draw is issued by the 3D executor, the experiment
   will have **zero** targeted submissions; interpret as unavailable,
   not as a genuine negative test.

The broad successful mode 3 (`ENABLE_EXECUTOR_SCENE_SUBMIT_TEST`)
remains available as a positive A/B reference.

All three modes:
- Keep every unselected draw on the normal threshold=256 schedule.
- Submit only *after* an eligible first draw at the identified phase
  transition (never in the middle of preparing a draw).
- Honor existing `gotoNext` protection against illegal mid-subpass
  submission.
- Keep the selected mode active after the 60-present metadata capture
  until the Strato app process exits; no diagnostic runs unarmed.
- Produce `capture_complete.txt` with
  `executor_experiment_mode=4`, `5` or `6`,
  `executor_experiment_draws`, and
  `executor_experiment_persists_after_capture=1`.
- Record each forced submission in `gpu_events.txt` as
  `EXECUTOR_BOUNDARY_SUBMIT`, and any ineligible `gotoNext`
  candidate as `EXECUTOR_BOUNDARY_SKIPPED`.

Observe gameplay for enough time to confirm whether the triangle
reappears. Send each capture ZIP, corresponding `emulation.log`, the
visual result and optionally average FPS. If
`executor_experiment_draws=0`, that test did not exercise the
intended transition and cannot disprove its relevance.

We should not ship these test hooks or a permanent extra submit as a
final fix. Even a working one-submit-per-frame diagnostic may mask
incorrect resource ordering or lifetime. After a boundary is
identified, inspect `SubmitInternal()`'s specific texture/buffer
dependencies, queue ordering, synchronization and frame ownership,
then fix the proven contract violation with regression tests.

**Stage 7 Android build / device validation not yet confirmed.**
No merge; Draft PR #414 remains the investigation branch.


## Stage 8 — HDR→postprocess producer/consumer correlation and submit isolation

### Confirmed prior outcome

The user tested all three Stage 7 modes at `Executor Flush Threshold=256`.
Each flag was confirmed by its own ZIP and log:

| Marker | Mode | Additional submits in 60 presents | Visible result |
|---|---:|---:|---|
| `ENABLE_EXECUTOR_DEPTH_SCENE_TEST` | 4 | 60 | Triangle remains |
| `ENABLE_EXECUTOR_SCENE_POST_TEST` | 5 | 240 | **Triangle disappears** |
| `ENABLE_EXECUTOR_VI_TARGET_TEST` | 6 | 59 | Triangle remains |

The mode-5 fix is **diagnostic only**. A sharp A/B points to extra
real `Submit()` calls after a non-HDR color draw following an HDR
1280×720 color draw; this `Submit()` happens **after the current
postprocess draw is queued**, not directly at the producer/consumer
handoff. Do not claim that a particular memory barrier is missing.

### New targeted modes, same Draft PR #414

Fully stop the Strato application between tests. Leave the emulator's
`Executor Flush Threshold` at **256**. In
`gpu_capture/pacman_world/`, create a **new empty**
`ARM_CAPTURE` and **exactly one** of these marker files:

| Marker | Mode | Behavior |
|---|---:|---|
| `ENABLE_EXECUTOR_POST_TRACE` | 7 | Full HDR→post candidate submission (mode-5 equivalent), with detailed traces |
| `ENABLE_EXECUTOR_POST_SLOT_1_TEST` | 8 | Submit only every 1st of four successive candidates |
| `ENABLE_EXECUTOR_POST_SLOT_2_TEST` | 9 | Submit only every 2nd of four successive candidates |
| `ENABLE_EXECUTOR_POST_SLOT_3_TEST` | 10 | Submit only every 3rd of four successive candidates |
| `ENABLE_EXECUTOR_POST_SLOT_4_TEST` | 11 | Submit only every 4th of four successive candidates |
| `ENABLE_EXECUTOR_POST_CONTROL` | 12 | Observe candidates with the **normal** 256 batching; no forced submissions |

Modes 7 and 12 should be tested first, to obtain comparable **good and
bad** trace data. Then use specific slot tests only after verifying
candidate IDs and order. If a run includes extra/missing candidates, a
slot may shift relative to the guest frame: modulo-four selection is a
diagnostic sampling heuristic, **not** a stable shader- or frame-ID
selector. A negative slot run cannot rule out an exact transition if
the candidate sequence shifts.

The GPU trace now records each `POST_CANDIDATE`, including:
- `ordinal` and `slot` within the **executor's sequence of
  postprocess candidates**;
- selected flag, `goto_next` status and whether a normal threshold
  flush would already have submitted;
- executor identity, submission ID, render-pass ID and current
  `FenceCycle` identity;
- color-attachment texture IDs, dimensions, formats, Vulkan layouts
  and their recorded cycles;
- first eight sampled-image IDs, dimensions, formats, layouts and
  cycles, plus total sampled count and input-attachment count;
- shader hashes (`vs`/`fs`), topology and draw type from the
  matching Maxwell3D guest draw when available;
- most recently observed HDR producer texture identity and whether
  an input is **that exact Texture object**. A different object can
  still alias the same guest memory, so false does not mean that
  no data dependency exists.

The trace is bounded to the first 60 host presentations. The selected
rendering mode persists until exiting the application. Captures
include `capture_complete.txt` with
`executor_experiment_mode=7..12` and
`executor_experiment_persists_after_capture=1`.
`executor_experiment_draws` counts extra submissions, so mode 12
should report **zero** extra submissions, while
`POST_CANDIDATE` metadata still appears.

Send the full ZIP (`gpu_events.txt`, `capture_complete.txt`),
matching `emulation.log` and the visible result/FPS per run.

**Only after** identifying a stable producer→consumer boundary should
a permanent patch alter memory dependency state, queue ordering or
resource lifetime. Preserve the existing diagnostic modes and the
confirmed working mode 5. Default code is unaffected when unarmed.
Stage 8 source integration is published; ARM64 build and device
validation remain unconfirmed until checked. Do not merge.


## Cross-game validation candidates (reported 2026-11-01)

User reports a visually similar inverted-triangle corruption in two more
Nintendo Switch games:

- **Marsupilami: Hoobadventure** (Unity).
- **Prince of Persia: The Lost Crown**.

These are **new visual reports only**. Neither title has an attached
diagnostic log or controlled Flush Threshold A/B in this investigation,
so do **not** claim that either shares PAC-MAN's HDR-to-postprocessing
dependency fault. In particular, do not infer the same Vulkan image
dimensions/format, shaders, content pipeline, or game engine.

### Minimal controlled validation per title

1. Use the same Strato fork and driver, identical level/camera and
   settings. Record a screenshot and a short log at the default
   `Executor Flush Threshold=256`.
2. Retest with `Executor Flush Threshold=0` and compare both
   graphical correctness and FPS. A positive result is evidence of
   submission-sensitive execution, but **not** proof of identical
   underlying cause. Fully restart the game between A/B runs.
3. Return to threshold 256. Only after step 2, optionally test the
   previously successful PAC-MAN
   `ENABLE_EXECUTOR_SCENE_POST_TEST` with `ARM_CAPTURE`.
   **Interpret `executor_experiment_draws=0` as an invalid/non-
   matching experiment**: its activation filter hardcodes the
   PAC-MAN-observed `1280x720` HDR source and expected pass pattern.
   Another title can use different dimensions/formats; a negative
   visual result with no candidate submissions proves nothing.
4. If all outcomes suggest the same root mechanism, introduce
   **metadata-only, general pass classification** first, by tracking
   actual resource producer/consumer identity, render attachment
   dimensions/formats and command execution cycles. Do not
   generalize by removing safeguards or globally inserting submits.

Cross-game regression criteria for an eventual structural correction:
PAC-MAN Re-PAC, Marsupilami Hoobadventure and Prince of Persia
The Lost Crown should render correctly with **threshold 256**,
without forced per-draw submissions or performance regressions.
Keep Stage 8 PAC-MAN diagnostics isolated; any general code change
must be proven on each game's actual trace. No merge.
