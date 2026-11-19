# Oceanhorn 2 — single APK for texture/mipmap residency, LOD and streaming (PR #469)

## Reframed bug (phone log + video, Nov 19, 2026)

The affected **geometry is visible at distance**, but its textures or correct
surface details only appear on approach. This is NOT adequately described as
"objects invisible". Eden shows the distant materials correctly. The previous
combined diagnosis reached 186 render summaries, detected significant texture
creation/guest backing traffic and a large number of sampled null fallbacks.

Previous experiments remain complete and should NOT be repeated:

- Force Visibility did not help.
- Force Query Positive successfully replaced sampled occlusion report values
  with 1 but did not fix the distant surfaces.
- Normal query results matched guest destination values in the sampled cases.

This does not prove that the cause is guest UE4 streaming, texture residency,
mip selection or an incorrect Vulkan binding. The point of this revision is
to discriminate between them using one run.

## One APK only, no behavior modifications

The existing Draft PR builds **`strato-pr469-arm64-release.apk`** with
`-PoceanVisibilityDiagMode=1`. Exactly one ARM64 DevRelease APK; no separate
Force Query APK, no Vulkan validation layer, no force-positive query results.

The following counters are logged per initially 16 and then 96 **executor
submissions**. Their intervals are NOT guaranteed to match video frames.
`seq` is per Maxwell channel; counters are lifetime-global relaxed atomics
and are reported as **deltas** across intervals.

| Marker | What it records |
| --- | --- |
| `[OCEAN-GEO]` | Draw paths, requested elements and zero-area scissor |
| `[OCEAN-STATE]` | Raster/depth state and sampled view slots |
| `[OCEAN-SHADERS]` | Four most-used stable shader-set fingerprints in first 32 entries |
| `[OCEAN-MATERIAL]` | First 32 shader + sampled backing/mip signatures, top 3 and untracked |
| `[OCEAN-MIPS]` | Draw-level counts of sampled base-mip 0/1/2/3+, single-mip views and invalid view ranges |
| `[OCEAN-FALLBACK]` | Split of sampled TIC pool-index, unsupported-format, unmapped, reserved/invalid descriptor, deliberate sampled/storage null bindless and out-of-range TSC |
| `[OCEAN-LOD]` | Count of **newly created** samplers, mipfilter None, maxLod <= 0.25, minLod > 0, nonzero bias, anisotropy |
| `[OCEAN-LOD-DETAIL]` | Sampled new sampler TSC index, mipfilter, real Vulkan minLod/maxLod/bias, anisotropy |
| `[OCEAN-TIC]` | Sampled decoded TIC virtual address/format/dimensions, requested mip chain and actual host view/backing range |
| `[OCEAN-SYNC]` | Guest→host upload calls/bytes, host→guest readback calls/bytes, decoded TICs, nonzero baseMip TICs, backing mip growth |
| `[OCEAN-STREAM]` | Existing buffer/texture creations and guest-buffer write/copy deltas |
| `[OCEAN-QRESULT]` | Previously verified sparse occlusion-query snapshots, forced=false |

`[OCEAN-LOD-DETAIL]` prints only the first 20 new samplers and every 128th
subsequent new sampler. `[OCEAN-TIC]` samples first 16 and every 256th
new TIC resolution, not every cached binding. These are bounded log probes,
not a full trace.

The sampled material signature hashes a texture's guest mapping pointer,
mip view range and dimensions with shader hashes. This is only stable in
**one emulator process**; compare far/near/far within the **same run**, not
signature IDs across different APK runs. The first 32 material combinations
are tracked; `untracked` is the number of other recorded draws.

Texture upload bytes use the host surface size when a CPU-dirty texture is
synchronized; these values are neither actual network/file asset streaming
nor total GPU VRAM use. Sampler creation counts do not reveal actual
pixel-fragment LOD or shadow tests. A sampled "null fallback" may be an
intentional unbound resource.

## One phone test; one log

1. Install the **latest successful Release** from this PR. Use the same
   game version, driver, save and options throughout.
2. Stand still facing a specific distant surface visibly lacking texture
   for about 10–15 seconds; note the time and take a screenshot.
3. Approach until its texture appears and stand still 10–15 seconds,
   note the time and take another screenshot.
4. Return to the original distance, keep orientation comparable, stand still
   10–15 seconds and take a third screenshot.
5. Send one complete `emulation.log`, those 3 captures and the
   approximate times for each phase.

## Decision points after the run

- A spike in `unmapped` sampled TICs, disappearing after approach, favors
  a missing mapping/residency hypothesis. Deliberate null bindless resources
  are tracked separately and are not by themselves bugs.
- Changes in `[OCEAN-MIPS]`, `[OCEAN-MATERIAL]`, `[OCEAN-TIC]` or
  `[OCEAN-LOD-DETAIL]` favor a mip range / sampler LOD / binding investigation.
  A baseMip=0 view can still sample lower-resolution mip levels dynamically.
- Guest→host upload spikes near the surface suggest delayed materialization,
  but do not prove game asset streaming. Compare with new textures/buffers.
- If these counters remain similar while the material visually changes,
  target a **specific** suspect shader, per-draw texture view or guest memory
  range for a frame capture / content comparison. Aggregate CPU logs cannot
  prove pixel LOD, correct texture bytes or identify individual UE4 actors.

No new Vulkan waits/fences/readbacks are introduced by this revision.
Prior query snapshots remain sparse. Native mode 0 production code is
unchanged. Keep PR Draft and **do not merge**.
