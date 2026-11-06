# Dead Cells — display upload provenance probe (diagnostic only)

The RenderDoc frame shows a corrupted RGBA8 1280×720 image before presentation.
Its source Vulkan upload buffer and destination image match byte-for-byte.
That does **not** identify the producer of the buffer; this temporary branch
investigates the upstream CPU-to-Vulkan texture preparation without changing
presentation, synchronization, resource ownership, or shader compilation.

## Capture scope

- **Build:** based on the unchanged master `e6da30c3799fd12549143a982b04ab14d1c2998f`, not on Draft PR #400.
- **Candidate:** 1280×720, single mip/layer, 3,686,400-byte RGBA8/BGRA8 host texture.
- **Provenance:** `Texture::SynchronizeHostImpl()` records the raw guest mirror bytes and the final prepared upload bytes, with guest tile mode, pitch, strides, formats, per-process texture pointer, aliases, and two FNV64 checksums.
- **Selection:** inspect up to 120 candidate uploads; save up to four uploads where fewer than 10% of output alpha bytes are 255, plus two reference uploads. At most 12 binary files, each at most 8 MiB.
- **Presentation:** record the per-process texture identity selected in `PresentationEngine::PresentFrame()` for up to 24 present calls.
- **Destination:** `getPublicFilesDir()/logs/deadcells-probe/` (JSONL and binary files). The data is reset on a new game launch. This diagnostic branch writes automatically; do not merge it as production code.
- **No inferred IOVA:** `GuestTexture::mappings` are spans into CPU mappings, not guest IOVA. Correlate to IOVA/TIC only with a supported Strato sidecar/debug name.

## Moto G100 test without ADB

1. Install the ARM64 DevRelease APK from this Draft PR.
2. Start Dead Cells **once**; reproduce the colored noise before its title menu.
3. Exit the game and return to the Strato game list (do not restart the game before export).
4. **Long-press the existing logs icon** in the search bar to share `deadcells-probe.zip`. A normal tap still shares `emulation.log`.
5. Send the probe ZIP with the captured RenderDoc RDC/AI ZIP already provided. The probe ZIP also includes `emulation.log` if it is within the size budget.

## Interpretation and limits

- A prepared staging sample matching the RenderDoc corrupted buffer identifies a path in Strato for upstream tracing.
- Raw guest and prepared staging bytes are intentionally **not** expected to match when swizzle/pitch or host format conversion is used.
- A raw guest layout that becomes invalid only after preparation suggests layout conversion or stale guest mapping; it still requires a controlled A/B or producer trace before a code change.
- FNV64 is a cheap diagnostic correlation hash, not cryptographic proof. Compare the exported binaries directly.
- An unexpected texture, a producer GPU write, or a different/earlier submit can remain outside this narrow probe.
- Logging/dumping can add startup latency. No CPU/GPU waits or barriers have been added. Do not infer root cause from the alpha percentage alone.

## First Moto G100 evidence — probe ZIP (2026-11-06)

- Driver: Turnip 24.0.99. The ZIP contains 10 logged uploads and 6 guest/staging binary pairs; no archive corruption.
- Every recorded guest texture is 1280×720, RGBA8 (Vulkan format 37), block-linear (tile mode 2), with guest allocation 3,932,160 bytes (padded) and linear staging 3,686,400 bytes. No sample reports an alias or freed guest memory.
- Independent Tegra GOB sector-order reconstruction (block height 16 GOBs, GOB 64×8 bytes/lines) reproduced **all 22,118,400 staging bytes exactly** from the six captured guest binaries. This rules out a mismatch in the observed block-linear deswizzle output; it does not prove the source guest contents are valid.
- Samples 1, 2 and 5 were blank in both guest and staging; using three precious capture slots for zero buffers blocked interesting later samples. Samples 3, 4 and 6 produced visible noise/tile-like patterns.
- A particularly valuable unsaved upload is **#7**, reporting 8,447 alpha=255 pixels of 921,600. The earlier independently captured RenderDoc event 1380 buffer had **exactly 8,447** such pixels, though the respective FNV64 hashes differ. This is a fingerprint candidate, not proof of resource identity across boots.
- Latest diagnostic probe now reserves the capture slots for **nonblank** uploads and keeps the first 600 present-texture identities instead of only 24.
- Errors in the same game run: **142** unsupported layer-create warnings, **5** `QueueBuffer` state warnings (`Free` instead of `Dequeued`), **5** matching `CancelBuffer` warnings, and a Turnip `VK_ERROR_OUT_OF_POOL_MEMORY`. None is yet causally linked to the pixel corruption.
- The original RenderDoc buffer does not match any of the six saved staging binaries byte-for-byte (different launch). Reproduce the same glitch with this revision and compare the new, nonblank #7-like guest/staging dumps.

The next trace should identify the producer/write site of the selected guest memory and correlate buffer-queue presentation identities; do not modify the render graph or resource sequencing before that evidence.


## Second Moto G100 evidence — near-identical pixels across boots

The updated ZIP includes samples 3, 4, 6, 7, 8 and 10 and 281 display-presentation records. The critical link is **sample #7**:

- `sample_7_staging.bin` vs earlier RenderDoc `upload_1380_buffer.bin`: **3,686,399 of 3,686,400 bytes identical**.
- The only difference is byte offset 18 (pixel x=4, y=0, B component): `0x50` vs `0x90`.
- All 921,600 alpha values are identical between captures (8,447 pixels have alpha 255).
- Sample #7 texture identity `0xb400006f8202f248` was presented 4 times (presentation indices 127–130).
- Independent block-linear deswizzle yielded zero byte differences for all six guest/staging pairs (22,118,400 bytes checked).
- Sample #7 `ObserveUpload` was called on the `Sky-Present` thread. This does not prove whether the guest wrote invalid pixels, chose an uninitialized surface, or failed to update a valid display buffer.
- Other observed warnings: 287 unsupported extra layer-creation attempts, three `QueueBuffer` (`Free` instead of `Dequeued`) and three `CancelBuffer` state warnings, and one Turnip `VK_ERROR_OUT_OF_POOL_MEMORY`. These are not confirmed as causes.

The new queue-side instrumentation records `QueueBuffer`-to-texture-to-NvMap association and the upload-side guest mapping pointer. It does not change queue state, guest bytes, GPU barriers, or presentation order.


## Writer-provenance probe — November 6 follow-up

Current evidence: 381 distinct NvMap handles reduce to 13 nonoverlapping guest allocations; 380 presents correspond in order to 380 queue entries, and the recurring corrupted surface is guest VA `0x1817500000`. This does not identify its writer.

The diagnostic branch now records:

- `cpu_write_trap_hits` in the upload record: count of successful **post-trap CPU write faults** after a texture mapping has been registered. A fault occurs before the actual guest store and may cover many writes; zero does **not** exclude CPU writes made before the texture was first trapped.
- `last_gpu_write_serial` and `ever_used_as_gpu_writer` on upload, plus `gpu_texture_write_intent` records from `Texture::RecordGpuWrite()`. These track classification/intent, not GPU completion.
- `dma_write_intent` for mapped destinations overlapping pre-registered display surfaces in Maxwell DMA, inline-to-memory and mapped Maxwell engine paths. Source engine labels do not establish the final resource producer, and not every possible memory writer has been instrumented.
- `fence_count`, `first_fence_id`, `first_fence_threshold` at `QueueBuffer` (first active fence only). `PresentFrame()` still waits using the original fence logic; no artificial GPU waits or barriers were added.
- `RegisterDisplayBacking` during `SetPreallocatedBuffer` and `AttachBuffer`, before queue, limited to 24 physical mappings (maximum 8 MiB each). Watch matches compare host mapped pointer intervals, not guest-VA number alone.

Important limitations: guest CPU code may fill a buffer before it enters this watch, guest memory can be modified by paths not covered above, and an event labelled `*_intent` is **not** proof of completed GPU work. A known recurring buffer signature is not automatically a valid frame. The next ZIP will show which source *classes* operated on the destination during the observed window. A source-specific correction still requires a controlled experiment and a frame that is known to be correct.

Testing: install the new ARM64 DevRelease from PR #425, launch Dead Cells once, reproduce the opening glitches, exit to Strato's library, and long-press the existing logs icon to share `deadcells-probe.zip`. Do not reboot Dead Cells before export; probe data resets on launch.


## Third Moto G100 capture — candidate writer and address-relocation evidence

The latest probe ZIP contains 19 uploads, 311 queue operations, 310 presents, 96 early GPU texture writer intents, and **zero observed DMA/GPU buffer writer intents**. All 310 recorded presents agree with corresponding queued texture identities; the final queue was not presented before export.

Candidate upload **#7** (`0xb400006f8205a1c8`, guest VA `0x1817120000`) again has **8,447 opaque-alpha pixels** and the same colored-corruption layout. Relative to RenderDoc event #1380, precisely **3,686,398 / 3,686,400 staging bytes match**. The two differing bytes are at offsets 18 and 5,138. Relative to the immediately preceding Strato capture, those same two staging bytes differ; the guest swizzled inputs differ at offsets 34 and 50.

A decisive structural clue: the first bytes include a little-endian pointer **`0x1817120038`** at staging offset 16 — exactly guest VA **plus 0x38**. The preceding Strato capture has `0x1817500038` at that position, and RenderDoc has `0x1817900038`. Additional pointer-like metadata appears at staging offset 5,136. The otherwise near-identical bytes strongly resemble structured memory being displayed as a framebuffer, rather than a valid image. They do **not** yet establish who wrote those bytes or why this allocation is used for display.

For upload #7, `cpu_write_trap_hits=0`, `last_gpu_write_serial=0`, `ever_used_as_gpu_writer=false`. Zero traps cannot exclude earlier untrapped CPU writes; the 96-slot GPU write-intent stream was exhausted early, though zero serial directly indicates this texture was never classified a GPU writer. The absent DMA entries exclude only the specifically watched and hooked mappings.

Warnings in that run: 319 ignored extra display-layer creations, 5 `QueueBuffer` `Free`-instead-of-`Dequeued` warnings, 5 matching `CancelBuffer` warnings, and a Turnip `VK_ERROR_OUT_OF_POOL_MEMORY`. Treat these as separate hypotheses, not causal attribution.

### Targeted, read-only stage comparison

The diagnostic now calculates a **sparse 6 KiB fingerprint** of each 1280x720 display allocation when attached/preallocated, then samples the existing **guest mirror** in `PresentFrame()` after the original fence wait and before `SynchronizeHost()`. It also records the unaligned 64-bit header word at byte offset 16, where the relocatable guest pointer appeared. The log fields are `watch_matched`, `sparse_hash_on_registration`, `sparse_hash_after_fence`, `sparse_sample_changed`, and `header_u64_at16_*`.

A changed fingerprint demonstrates a change in sampled bytes between these two observation points, not which writer caused it. An unchanged fingerprint does not prove no changes outside those sectors. The initial registration probe reads guest-mapped CPU memory and may affect trap-protected behavior; compare performance/glitch behavior against the previous probe before interpreting results. No explicit GPU wait, barrier, or queue mutation was added.

Collect the next ZIP on the same driver and gameplay opening; only then decide whether to trace the allocator, an earlier CPU producer, or guest layer creation.
