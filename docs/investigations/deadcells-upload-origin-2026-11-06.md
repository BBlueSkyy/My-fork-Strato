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
