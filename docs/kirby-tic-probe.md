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
- `nvMapMatch`: the nvhost-as-gpu map history contains a record covering the
  slot **and** its recorded host backing address matches the current GMMU
  translation of the slot. A false value means only that the bounded history
  could not verify it, **not** that no CPU mapping exists.
- `nvMapId`, `nvMapBackingAddress`: the NvMap handle ID and the handle's
  associated memory address plus the subrange offset (this may refer to nvdrv
  transfer memory, not necessarily a conventional user VA). Do not confuse
  these with the image IOVA encoded inside the invalid TIC.
- `gpuMapBase`, `gpuMapBytes`, `mapSource`, `mapGeneration`: metadata
  recovered from NvMap-to-GMMU mapping operations. Sources: 1=fixed
  MapBufferEx, 2=dynamic MapBufferEx, 3=subregion remap, 4=sparse remap.
  They establish mapping provenance, **not the identity of a writer**.

Only the first 96 distinct events per process are printed; identical events
are deduplicated per thread. Restart the game to reset this limit. The
`became-valid` observation tracks recent reserved slots on the same
compilation thread (a bounded 32-slot history), not every possible mutation.

## Bindless handle origin — narrow shader-side check

The latest capture verifies that the invalid TIC at `0x54A070000`
comes from NvMap handle 28, backing address `0x18A2CF3000`. The
backing remains identical over the observed pool refreshes, and no
DMA/I2M producer was identified. Therefore, before trying CPU page
traps, the probe now asks whether *the emulator synthesized TIC index
zero* due to a missing constant buffer.

`ReadBindlessHandle` currently returns raw zero when the primary
constant buffer has no bound view. It may also legitimately read zero
from a bound guest constant buffer. These cases are semantically
different: the diagnostic preserves the existing return values and
records only a small thread-local breadcrumb when the resulting
bindless handle has texture index zero.

The next `[TIC-PROBE]` event with `site=sampled` adds:

- `bindlessMatched`: true only if the last handle read on the same
  thread has TIC index zero and matching channel sequence;
- `bindlessRaw`, `primaryCbuf`, `cbufOffset`, `arrayIdx`:
  the actual bindless bits and their constant-buffer location;
- `primaryBound`, `primaryValue`: whether a guest CBUF was
  bound, and the 32-bit value obtained when present;
- `secondaryUsed`, `secondaryBound`, `secondaryValue`: optional
  second buffer used to construct a combined handle.

A true `bindlessMatched` together with `primaryBound=false` is
specific evidence that Strato generated the zero handle in that
runtime path. `primaryBound=true` and `primaryValue=0` instead
means the handle comes from readable guest memory. Neither alone
proves whether the game *intended* to sample that slot. A false match
is inconclusive, and compile-time type lookup is still independent
of this runtime breadcrumb.

This is diagnostic-only code. No guest memory writes, page traps,
Vulkan synchronization, shader compilation or cache semantics change.

## Mapping provenance — next diagnostic pass

After the first captured Kirby TIC at `0x54A070000` contained eight copies
of `0x7FBFFFFF`, the DMA/I2M-only capture produced **zero**
`[TIC-WRITER]` candidates. To avoid intrusive CPU traps, the next pass tracks
successful `nvhost-as-gpu` `MapBufferEx` and sparse `Remap` calls. The
process keeps at most 4096 mapping records. When the same invalid TIC is read,
the probe verifies the record against the **actual host byte pointer** obtained
from the GMMU mapping before reporting `nvMapMatch=true`.

Unmapping a range invalidates its recorded provenance. Mapping history is
bounded, and some mapping paths are outside `AsGpu`; therefore a missing
match is inconclusive. The diagnostic stores host pointers only for equality
checks and never logs them. It does not force GPU submissions, change page
permissions, install write traps or read unrelated memory.

**Follow-up test:** boot Kirby with the new APK, enter gameplay and send
the full `emulation.log`. Look for `nvMapMatch` and
`nvMapBackingAddress` beside `[TIC-PROBE]`. This narrows down a CPU memory
region for a later *selective* write-provenance check without asserting the
source prematurely.

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
