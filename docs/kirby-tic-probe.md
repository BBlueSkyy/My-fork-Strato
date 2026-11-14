# Kirby / TIC 15 — diagnostic build based on PR #454

Base: `d951ca35102b2a0fd91188ec2e08e0ca8c9a41b6` (PR #454 audit).
This is **instrumentation only**. It does not change the sampled compatibility
fallback or claim to repair reserved TIC semantics. Do not merge this branch.

## Capture

In the Kirby test, use the separate artifact
`strato-kirby-tic-probe-arm64-release.apk` from the
[Kirby TIC Probe ARM64 workflow](../../.github/workflows/kirby_tic_probe.yml).
This build does **not** include a Vulkan validation layer.

Collect `emulation.log` and search for `[TIC-PROBE]` **and**
`[TIC-WRITER]`. The next capture should include the whole process from boot:
the important writer might execute before the first invalid TIC read.

`[TIC-WRITER]` records two bounded classes of candidate events:

- `MaxwellDMA`: an overlapping destination for the first slot of the current
  pool, or a remap operation with `ConstA == 0x7FBFFFFF` selected in its
  destination swizzle. The logged `spanBytes` is a linear or pitched
  **bounding span**; a positive overlap can be a false positive if the line
  gaps are involved. Block-linear multiline copies are not checked for overlap
  unless a suspect constant is supplied.
- `I2M`: a possibly overlapping destination or eight consecutive
  `0x7FBFFFFF` words at the **start** of its inline payload. It does
  not search every word in a large upload.

The first TIC slot address is read from the live header-pool register when
`TexturePoolState::Flush` runs; there is no hardcoded Kirby IOVA. Both
writer types are capped at 48 log entries per process. These hooks do not
observe arbitrary CPU/NCE memory stores, Vulkan render/compute shader writes,
or earlier DMA operations whose output was neither recognized as a suspect
constant nor targeted after the first pool watch was armed. These are
candidate commands, not confirmed completed writes. **An absence of writer
logs does not disprove any writer hypothesis.**

`[TIC-PROBE]` lines retain their existing fields:

- `site`: shader compile, sampled binding or a previously-invalid slot that
  later became a valid target during another shader compilation.
- `shader`, `stage`, `shaderHash`: active graphics or compute compilation.
  Runtime sampling may say `runtime`; it has no shader-stage identity.
- `index`, `type`, `seq`, `poolGen`: TIC slot and CPU-side channel
  sequence / pool refresh generation; neither is a guest writer ID.
- `poolIova`, `slotIova`, `slotContiguousBytes`: TIC table location and
  number of bytes contiguous in the first GMMU mapping, clamped to 32.
- `imageIova`, `imageBaseMapped`: address encoded *inside* that TIC and a
  **base-byte** GMMU check. No footprint/split-range proof is implied.
- `raw`: all eight 32-bit words (32 original descriptor bytes), in host
  little-endian order, from the already mapped TIC object.

Only the first 96 distinct events per process are printed; identical events
are deduplicated per thread. Restart the game to reset this limit. The
`became-valid` observation tracks recent reserved slots on the same
compilation thread (a bounded 32-slot history), not every possible mutation.

## What this cannot determine

A snapshot is not a historical record of GPU writes. It cannot by itself
identify which engine wrote the TIC, whether a pending GPU write was submitted,
or whether the pool crosses GMMU mapping boundaries in a way that invalidates
`TexturePoolState::Flush`. Do **not** declare the root cause from
`imageBaseMapped=false` alone.

To determine a producer, first correlate the exact slot address/raw bytes
and sequence with GMMU mapping and I2M/GPU-memory write activity. Add more
targeted instrumentation only after such a correlation is established.

The parent PR's four cache-specialization assertions are still failing.
This branch has its **own push-triggered APK workflow** so a diagnostic APK
can be built without pretending the parent's semantic merge gate passed.
This does not authorize merging #454 or this diagnostic branch.

## Quick sanity checks

```sh
python3 tests/gpu_tic_probe/run.py
python3 tests/gpu_null_storage_image/run.py
python3 tests/gpu_tic_validation/run.py
```
