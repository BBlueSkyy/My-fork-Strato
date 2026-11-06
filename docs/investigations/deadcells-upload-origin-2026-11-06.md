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
