# Qualcomm proprietary Vulkan descriptor diagnosis (SpongeBob: The Cosmic Shake)

This **Draft diagnostic** records `[PROP_DESC]` lines. It does **not** fix the
graphics, rewrite descriptor bindings, round texel view offsets or change
shader formats. Existing Turnip and Qualcomm behavior is preserved.

The previous Qualcomm 0.777.0 / Adreno 650 log showed:
- `VUID-VkBufferViewCreateInfo-offset-00926`: unaligned texel buffer views
  (minimum alignment 64 B; observed offsets include 6896 and 8176).
- `VUID-vkCmdDispatch-None-02699`: bindings 2 and 3 expected FLOAT, were
  bound to R32_UINT.
- Segmentation fault in Qualcomm `vulkan.ad7770.so` while calling
  `vkUpdateDescriptorSets` from `SetDescriptorSetCmdImpl<false>::Record`.

## Instrumentation
- `VIEW_CREATE`: offset/range, format, alignment, buffer handle, emitted
  **before** creating the VkBufferView; `VIEW_READY` contains the view handle.
- `TEXTURE_BUFFER` and `IMAGE_BUFFER`: TIC index, raw TIC format,
  texture IOVA, Vulkan TIC format (where supported), requested shader image
  format, `Typeless` flag, chosen Vulkan view format and the **same view
  handle** used by descriptor writes.
- `PRE_UPDATE`/`WRITE`/`PRE_COPY`/`POST_COPY`/`PRE_WRITE`/
  `POST_WRITE`: descriptor set, bind point, bindings 2/3 and texel-buffer
  writes, type, first VkBufferView handle, and the exact side of the driver
  call that last completed.

For a focused report:
1. Install the ARM64 DevReldebug APK from this Draft branch.
2. Select **Qualcomm proprietary** driver, open Cosmic Shake until it crashes.
3. Send the new `emulation.log`. Look for the last
   `[PROP_DESC] PRE_WRITE` without matching `POST_WRITE`; inspect the
   `WRITE` lines sharing its `seq`, and match `firstView` with an
   `IMAGE_BUFFER` or `TEXTURE_BUFFER` line by `view`.

Logging is **automatically enabled** in this diagnostic branch only, with
bounded initial records and sparse sampling thereafter; it is not suitable
for merge or general performance measurements. No Actions watching.

These logs can support a root-cause hypothesis but are not proof by
themselves: a crash inside a driver can also reflect earlier corruption,
invalid lifetime or synchronization. Inspect the Vulkan validation results
before implementing a fix.

## Second diagnostic revision

The first Qualcomm capture successfully found 34 misaligned view creations
of 69, and repeatedly correlated TIC 504 (`ticVkFormat=74`,
`shaderFormat=0 Typeless`, `viewVkFormat=98`) with a storage-texel
descriptor in binding 5. This is a **format divergence**, not proof of
the precise crash cause.

The old `PRE_WRITE` sampler stopped reporting every update after
sequence 3072; the last sampled seq=6400 completed around 62.5ms before
the SIGSEGV. That sample gap prevented identifying the crashing call.
In this revision **every** `PRE_UPDATE`, `WRITE`, `PRE_WRITE`,
and `POST_WRITE` is logged for all Vulkan descriptor types, with
pointer identities for each write. The per-update descriptor summary is
limited to 24 entries, but every update receives the pre/post markers.
Logs are intentionally verbose; do not use this branch for FPS tests.
The `VIEW_CREATE` instrumentation remains unchanged. No speculative
runtime correction is included.

## Third diagnostic revision (Release, Qualcomm 0.777.0)

New full-trace log pinpoints `PRE_WRITE seq=6368` with no matching
`POST_WRITE`; the stack faults in Qualcomm `vkUpdateDescriptorSets`.
That batch has four uniform buffers (binding 0) and **three uniform
texel-buffer views (binding 4)**, with zero copies. This is the *only*
multi-element type-4 write in the logged run. Its first view was
created as format 99 at offset 2560 (64-byte aligned), so evidence
does not establish that the first view is malformed. The remaining
two handles were not yet logged.

The diagnostic now logs all up to 16 members of multi-element texel
and buffer descriptor writes (`TEXEL_ELEMENT`, `BUFFER_ELEMENT`), to
compare all seven handles and buffer ranges against creation traces.
No shader, view, descriptor, binding, or runtime semantics are changed.
