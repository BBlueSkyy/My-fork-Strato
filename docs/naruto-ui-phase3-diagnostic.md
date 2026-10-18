# Naruto UI: targeted mini-RenderDoc diagnostic (PR #336)

This branch is diagnostic-only, **not** a rendering fix. No title-ID or shader-hash
conditions influence guest GPU emulation. Baseline: the Oct 17 capture of Naruto Storm 4
showed a logo in present source 0, absent in sources 1–3; all four sources were captured
before copying to the Android swapchain. The supplied emulation.log was Silksong,
not Naruto. The previous capture exhausted 160 MiB on 29 early full-screen snapshots.

## What phase 3 changes

* A **blended draw sampling a compressed image** identifies a *candidate* sprite/UI
  output (heuristic, not proof). Preserve its guest backing ranges under a lock.
* Later draws/blits sampling those ranges mark their color destinations as downstream
  candidates, even across command executors, by guest-memory range overlap.
* Readbacks occur **after render pass endings**; only candidate and downstream color
  targets spend the bounded snapshot budget (up to 4 per approximate presentation
  interval and 16 total; hard cap still 160 MiB). No early scene-only snapshots.
* In active captures, Fermi2D and MaxwellDMA record source/destination IOVA and
  transfer parameters. The present thread records each real frame ID, source texture,
  guest backing and fence cycle identity, separately from the approximate interval.

This is a targeted hypothesis test. If the UI is not drawn using blended compressed
assets, a candidate may not be selected. If there are no pass snapshots, inspect
`gpu_events.txt` for `UI_CANDIDATE` before broadening the heuristic.

## Android reproduction

1. Build and install the *new* reldebug APK for this branch (the old Oct 11 APK
   does **not** contain phase 3).
2. Reproduce Naruto Storm 4 menu flicker.
3. In the Strato public files folder, rename
   `gpu_capture/naruto_ui/ARM_CAPTURE.rename_this_file_when_ready` to `ARM_CAPTURE`.
4. Remain on the flickering screen until four presentations finish. Zip the new
   `capture_###` directory **after** the `MINIRD_NARUTO_PHASE2 wrote snapshot`
   readback messages stop. `capture_complete.txt` alone does not guarantee
   every asynchronous snapshot file has been written.
5. Include the `emulation.log` from the **same Naruto session** (check title ID).

Compare captured final presentation sources against `UI_CANDIDATE`,
`PHASE2_SNAPSHOT`, `FERMI2D`, `MAXWELL_DMA` and `PRESENT_SOURCE` events.
Identify the first divergence, not just the first missing final logo.
The `present_index_approx` tag is **not an exact frame-boundary timestamp**.
GPU readbacks perturb timing and cannot themselves establish a runtime fix.

Host regression test: `bash tests/naruto_ui_capture/run.sh`.
