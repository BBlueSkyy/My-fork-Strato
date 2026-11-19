# Oceanhorn 2 — one-APK render-path investigation (PR #469)

## Known evidence

- In Strato, distant scene objects become visible only on approach; Eden renders them at a distance.
- The earlier PR #469 Force Visibility (render predicate override) did not fix them.
- Force Query Positive demonstrably wrote 1 into the guest query report buffer, but the distant objects remained missing.
- Release control sampled matching actual GPU query results and the bytes written to the guest destination.

These tests narrow the occlusion/render-predicate hypotheses. They do not
prove guest Unreal Engine culling, LOD, streaming or a host rasterization defect.

## One APK only

**`strato-pr469-arm64-release.apk`**, native macro
`-PoceanVisibilityDiagMode=1`. The former second APK is no longer built.
The build does **not** override GPU predicate state or occlusion-query values.
No Vulkan Validation Layer is packaged.

The following lines appear periodically in the regular `emulation.log`.
The initial interval is 16 and later intervals are 96 **executor submissions**,
not rendered frames. Their `seq` values identify intervals per Maxwell channel.

- `[OCEAN-GEO]`: regular, indexed, indirect, inline-index and fixed-function
  draw paths; requested element/instance counts and zero-size scissors.
  Indirect calls report requested draw-command count, not the GPU-computed
  number of visible primitives.
- `[OCEAN-STATE]`: aggregate bound depth-test/write, cull,
  rasterizer-discard, alpha-test, predicate, RT/depth attachment counts;
  sampled texture descriptor slots, null slots and nonzero/partial mip views.
  These are state counts **not GPU-measured fragment rejection**.
- `[OCEAN-SHADERS]`: stable combined shader-stage hashes observed in
  each interval, and their draw frequencies. Only the first 32 unique
  combinations are counted exactly; the `untracked` field identifies
  excess combinations. The four busiest within that bounded group are printed.
- `[OCEAN-STREAM]`: creation of guest textures and buffers, their sizes,
  guest buffer Write/Copy traffic, sampled TIC fallback counts, and
  guest-render disabled counts. These provide streaming/allocation
  indicators but are not a direct window into UE4 asset requests.
- Prior `[OCEAN-QREPORT]`, `[OCEAN-QRESULT]`, `[OCEAN-COND]`
  remain, with `forced=false`.

All resource counters are relaxed atomics. No new GPU fences or readbacks
are introduced by the renderer diagnostic; the previously implemented
sparse query sample (first/every 8192nd report) remains. Per-draw work
is bounded. It can still change FPS, so use this APK for diagnosis only.

## One on-device test / one log

1. Install this Release from the latest successful PR #469 workflow.
   Keep game update, driver and settings unchanged throughout.
2. At a location where an object is **missing at distance**, stand still,
   leave the camera facing the object for 10–15 seconds and take a screenshot.
   Note approximate wall-clock time.
3. Approach until the object appears; stand still for 10–15 seconds
   facing it. Note time and capture another screenshot.
4. Return to the original distant position; stand still for another
   10–15 seconds and capture a third screenshot.
5. Send **one** complete `emulation.log`, three screenshots and approximate
   times for the far/near/far phases.

## Interpret carefully

- A fall or rise in specific shader keys and draw counts at approach is
  evidence of a render-workload change, but does **not** uniquely identify
  Unreal Engine actors or establish CPU-side culling.
- If draw counts stay stable while mip, null-descriptor or GPU depth/cull
  states shift, investigate those specific paths. Shader-side branching,
  indirect argument buffers and depth/stencil fragment rejection are not
  directly measurable by these CPU-side counters.
- For a definitive mapping of individual invisible objects to draws,
  a targeted frame capture, shader instrumentation or guest UE4 tags
  would be required after the first log narrows the suspect workload.

Keep this PR Draft and **do not merge**. Production mode 0 is unchanged.
The old force-positive implementation remains behind opt-in macro mode 2
for historical comparison, but its APK is not built/published.
