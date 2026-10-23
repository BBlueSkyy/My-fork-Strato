# First anomalous UE4 HDR draw — guest/host input capture

**Diagnostic Draft. Not a rendering correction. Do not merge.**

This rebases the necessary mini-RenderDoc infrastructure from PR #379
onto the current master (including modern texture layout/alias handling),
without replacing master GPU/TexMan implementations with old copies.

## Scope

With `gpu_capture/ue4/ENABLE_HDR_FIRST_BAD` present, the capture now
samples only the **first seven HDR draw ordinals** on the first selected
`B10G11R11UfloatPack32` backing (one baseline plus after-draw
readbacks). Previous captures observed the first anomalous draw at
ordinal 1 (fragment `EA18B70958415FF3`) and a first green-channel
NaN by ordinal 7 (fragment `1B8CB95A440E18C1`). These are *observed
identities*, not hard-coded gameplay workarounds.

In addition to shader binaries, render-target data, descriptor metadata,
resolved views, sampled textures and CBUF slot sizes, the active
first-HDR mode now records:

- `HDR_GUEST_STATE ordinal=1|7` for each resolved sampled view:
  guest first mapping, guest span size and mapping count, dirty state,
  `memoryFreed`, alias state, readback serial and pin count.
  These metadata reads do **not** access discarded/trapped guest bytes.
  If a lock is busy, the diagnostic explicitly logs `state_lock_busy=1`.
- A bounded snapshot of up to **12 distinct Vulkan sampled inputs**
  immediately before draw 1, named `material_input_1_<index>`.
  Each color input must be one of the already supported 2D backings,
  have TransferSrc and defined layout, and be at most **4 MiB**.
  Images that are unsupported are skipped with metadata. These snapshots
  are from Vulkan, not direct copies of guest memory.
- HDR target snapshots before draw 1 and after draws 1–7, subject to
  the existing global focus quotas and normal GPU scheduling.

**Important limitation:** the current capture does not dump complete
constant buffer contents or record the *first CPU/DMA write* to the
guest allocation. A mismatch in inputs must be traced to its producer
before a renderer change can be considered proven.

## A/B test with the same APK

1. Install this PR's **ARM64 DevRelease** APK; use the same Turnip
   driver, same game version, same scene and preferably the same shader
   cache setup for both runs.
2. Keep **Fast GPU Readback Hack OFF**. Create an empty file at
   `gpu_capture/ue4/ENABLE_HDR_FIRST_BAD` inside the Strato public app
   files (before the game boots). Ensure
   `ENABLE_UPLOAD_PROVENANCE` is **absent**. Restart Strato.
3. Set **Free Guest Texture Memory OFF**, boot SpongeBob, enter the
   broken scene and arm by renaming
   `gpu_capture/ue4/ARM_CAPTURE.rename_this_file_when_ready` to
   `ARM_CAPTURE`, or create an empty `ARM_CAPTURE` there.
4. After `capture_complete.txt` appears, wait for pending
   asynchronous `MINIRD_UE4` snapshot writes. Save this
   `capture_###` directory as `off_capture.zip`, plus
   `off.log` and a screenshot.
5. Stop the game fully. Switch **Free Guest Texture Memory ON** and
   repeat the **same capture** in the same scene. Save
   `on_capture.zip`, `on.log` and a screenshot.
6. Remove the enable marker after testing and restart Strato.

## Comparison target

Compare `HDR_DRAW_IMAGE` and `HDR_GUEST_STATE` at draw 1, the
`material_input_1_*` readbacks, `HDR_DRAW_CBUF` references and
`hdr_scan_after_1` output. Identify the *first* sampled input
whose Vulkan bytes differ between equivalent draws, and decide whether
the difference originates in guest data, layout/transfer/aliases or a
GPU-produced render target. If those inputs agree, investigate the
actually referenced CBUF values and shader semantics next.

The reported `hdr_first_suspect.txt` and R11G11B10 >1000 threshold
are diagnostic heuristics, **not proofs** of a bad shader. Multiple
passes, input states, ASLR, GPU drivers and draw-order variations may
invalidate a naïve draw-ordinal comparison.

No RelDebug, no mandatory new tests, no global logging at runtime,
no changes to texture ownership, shader execution, or FreeGuest behavior.
This instrumentation can reduce FPS **while armed**; it is not
appropriate for performance measurements.
