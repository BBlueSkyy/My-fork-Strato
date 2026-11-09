# Fire Emblem: Three Houses — Vulkan descriptor crash (2026-11-09)

## Scope
- Game: Fire Emblem: Three Houses (`010055D009F78000`), **v1.0.0**, no update or DLC loaded in the supplied session.
- Device: Moto G100 / Snapdragon 870 / Adreno 650 / Android 12.
- Initial driver: Qualcomm proprietary, Vulkan 1.1.276, driver 0.777.0.
- Reproduction source: user-provided `emulation.log` (197 lines).
- Baseline: `master` at `f0a95dd80eb150e81d16e1b0fa88c3a812a820c8` (includes merged PR #430).

## Confirmed failure
The title initializes, loads `main.nso` and reaches HOS execution. It then faults on the `Sky-CmdRecord` thread:
```
ERROR | 4259782 | Sky-CmdRecord | Run: Signal: Segmentation fault (PC: 0x0)
* 0x0
* qglinternal::vkUpdateDescriptorSets(...) from vulkan.ad7770.so
* skyline::gpu::interconnect::CmdHolder<
    SetDescriptorSetCmdImpl<false>>::Record(...) from libskyline.so
```
Audio latency errors (`-4`) precede the crash but are not proven causal.

## Working hypotheses (not confirmed)
1. Invalid descriptor contents (image view, sampler, buffer handle, offset or range) or layout/binding mismatch.
2. Lifetime/reuse of Vulkan descriptor resources while the record thread updates a set.
3. An Adreno proprietary driver defect when processing otherwise valid writes.
4. A synchronization hazard upstream that exposes a stale descriptor.

The stack only identifies the **native Vulkan descriptor update**. It does not establish which hypothesis is correct.

## Diagnostic build plan
- This PR builds **only DevDebug**. Its CI packages the pinned Khronos Validation Layer 1.4.363.1 into `app/src/debug/jniLibs/arm64-v8a`, enables synchronization validation (following PR #429), and emits bounded `FE3H-DESC BEFORE/write/AFTER` records around non-push `vkUpdateDescriptorSets` calls.
- Following the first device capture, `FE3H-DESC BEFORE/AFTER` is emitted **for every update**; `COPY_BEGIN/END` and `WRITE_BEGIN/END` isolate the exact crashing driver call. Per-binding details are emitted for the first 2,048 updates and then every 128th call.
- Additional Debug-only probes identify unaligned texel-buffer views and 1D shader/TIC/view mismatches. They report the source without changing the guest offset, type or descriptor.
- First captured Vulkan VUIDs: `VUID-VkBufferViewCreateInfo-buffer-02750` (offset 4218144, alignment 64); `VUID-vkCmdDispatch-viewType-07752` (shader `cs_tex2_28` expects 1D, descriptor is 2D). Also repeated zero-width viewport warnings. None is proven to be the cause of the native Qualcomm `vkUpdateDescriptorSets` crash; first capture's last detailed pair was seq 384, but the crash happened later.
- Logs are **diagnostic only**. No descriptor rewriting, forced waits, resource lifetime changes, or GPU/compiler behavior change is included. The prior CI run compiled DevDebug but failed packaging verification because the layer had been installed outside an Android JNI source set; its path is corrected here.

## Device test
1. Download the **ARM64 DevDebug Validation** APK from the newest CI run, install it, and use the Qualcomm proprietary driver with the same game version/settings as the supplied log.
2. Launch Fire Emblem: Three Houses once, stopping at the crash; save the full `emulation.log`, including `Vulkan validation enabled`, the last `FE3H-DESC WRITE_BEGIN/WRITE_END` pair, `FE3H-TEXEL`, `FE3H-TEX-1D` and Vulkan VUIDs.
3. If the same crash is reproduced, repeat with a Turnip driver **without clearing the shader cache between tests** when possible and identify the driver version in each log. The two logs must be labeled by driver.
4. If DevDebug does not reproduce, do not assume that a debug-only success proves a correction. A release comparison will be considered only after collecting device evidence.

If the debug build cannot load the layer, record its startup error rather than interpreting the failure as a game regression.

## Second device capture — exact driver failure (2026-11-09)
The newer DevDebug log (2,438 lines) confirms that the driver crash is exactly inside the **graphics** `vkUpdateDescriptorSets` call with `FE3H-DESC seq=303`: `WRITE_BEGIN` appears but `WRITE_END` does not. This update contains 10 writes: one UniformBuffer, one UniformTexelBuffer with **three** view elements (binding 1), and eight CombinedImageSampler writes (bindings 4–11). The preceding 302 descriptor updates returned normally. Validation shows an unaligned texel-buffer creation at offset `4215264` (32 bytes modulo 64), a 1D shader/2D view mismatch (TIC 286; `cs_tex2_28`), and unrelated initial viewport/swapchain VUIDs. Neither the unaligned view nor the dimension mismatch is proven to be among the ten crash descriptors.

The new targeted diagnostic logs the actual `VkBufferView` handles, returned views from misaligned creations, and the image/sampler handles of descriptor batches containing multi-element texel arrays. The data will establish whether the crashing batch uses one of the invalid-alignment views or contains a null/invalid handle. No descriptor contents are rewritten and no global synchronization is added.

## Third device capture — direct invalid-view correlation (2026-11-09)
The new DevDebug log (2,713 lines) crashes in `FE3H-DESC seq=337`, which has the same graphics descriptor layout as the earlier crashes. The write at binding 1 is `UniformTexelBuffer` with three views. Two of its actual handles match the exact `VkBufferView` returned for **VUID-invalid offset** creations earlier in the same log:
- `0x2380000000238`: `VkBuffer` `0x21D000000021D`, format `R32Uint`, offset `6624`, which is **32 bytes** out of the required **64-byte** alignment.
- `0x21E000000021E`: same backing `VkBuffer`, format `R32G32B32A32Sfloat`, offset `2528`, likewise remainder **32** modulo 64.
- `0x23B000000023B`: third texel view; no misalignment warning associated with its handle in this capture.

This establishes a **direct association** between two invalid view creations and the exact native descriptor update where Qualcomm crashes. It does not yet prove the invalid views caused the driver segfault, which is why the next experiment isolated the ten writes (result below). The Qualcomm Vulkan 1.1.276 extension list in the captured device log does not contain `VK_EXT_texel_buffer_alignment`.

### A/B experiment (superseded)
The DevDebug build issued the same ten writes as ten sequential calls (`FE3H-DESC WRITE_ITEM_BEGIN/END`). That instrumentation has been removed in the fix PR; it is recoverable from PR #433 (`c479de15`).

## Fourth capture — A/B result (2026-11-09, `emulation.log`, 2,530 lines)
Build: PR #433 head `c479de15` (split mode), Adreno 650, Qualcomm proprietary Vulkan 1.1.276, driver 0.777.0, validation layer 1.4.363.
- `FE3H-DESC seq=314` (graphics, 10 writes) was split. Item 0 (`UniformBuffer`, binding 0) returned (`WRITE_ITEM_END index=0`). Item 1 (`UniformTexelBuffer`, binding 1, count 3) logged `WRITE_ITEM_BEGIN` and never `WRITE_ITEM_END`. Items 2–9 never ran.
- `SIGSEGV @ 0x730000029D`, PC in `vulkan.ad7770.so`, stack `qglinternal::vkUpdateDescriptorSets` called from the validation layer, called from `SetDescriptorSetCmdImpl<false>::Record`. The crash is therefore inside the driver call for that single write, **not** in the batch of ten and not in the validation layer.
- The three views (`0x1770000000177`, `0x15D000000015D`, `0x17A000000017A`) are non-null, and the validation layer reported no object/usage/range VUID for them. The only validation messages in the log are zero-width viewports (`VUID-VkViewport-width-01770`), the swapchain `preTransform` and presentable-image layout-transition notices, and one `VUID-vkCmdDispatch-viewType-07752` (compute, unrelated to this draw); none concerns a descriptor write, a `VkBufferView` or a `VkBuffer`.
- **The log contains no `FE3H-TEXEL` line and no `VUID-VkBufferViewCreateInfo-buffer-02750`.** The probe logs every texel buffer view request whose offset is not a multiple of `minTexelBufferOffsetAlignment`, so no misaligned view was requested in this session before the crash. The same crash still happened.
- Earlier descriptor updates in the same log passed: `seq=312` has a `UniformTexelBuffer` write with count 1 (binding 3), and `seq=300–310` contain `UniformBuffer` writes of count 2 spanning two bindings. `seq=314` is the first update in the log whose texel buffer write has `descriptorCount > 1`.

## Root cause assessment
Layout of the crashing update, derived from the log and `MakePipelineDescriptorInfo`: binding 0 uniform buffer; bindings 1–3 are **three** uniform texel buffer bindings of one element each (the next write, bindings 4–11 combined image samplers, starts at 4); `Pipeline::SyncDescriptors` emitted them as **one** `VkWriteDescriptorSet` (`dstBinding=1`, `descriptorCount=3`) that spills across bindings 1–3.

1. **Batched multi-binding texel buffer write (fix applied).** Valid Vulkan, but only the Maxwell3D graphics path produced it. The compute path (`kepler_compute/pipeline_manager.cpp`) already writes one descriptor binding per `VkWriteDescriptorSet` for every type and works on the same driver. The existing Adreno quirk `needsIndividualTextureBindingWrites` documents the same class of bug for combined image samplers (batched writes across bindings), but texel buffers were never covered. Every captured crash is the first texel buffer write with count 3 (`seq=303`, `337`, `314`), and the single-binding texel write passes. This is a strong correlation, but it has **not** been proven by a direct experiment: the A/B only proved the crash is in that one write. The first device run of this fix is the experiment.
2. **Misaligned texel buffer views (not changed).** Captures 1–3 recorded views at offsets `4218144`, `4215264`, `6624` and `2528` (remainder 32 modulo the 64-byte `minTexelBufferOffsetAlignment`, `VUID-VkBufferViewCreateInfo-buffer-02750`), and the device lacks `VK_EXT_texel_buffer_alignment`. That is a real conformance violation, but capture 4 crashes with no misaligned view, so it is not the cause of this crash and is not confirmed as a cause in any capture. Guest texel buffers are only guaranteed 32-byte aligned, so a correct fix has to place the data at an aligned offset (copy/relocate); rounding the offset would change which texel the shader reads. That change affects every game that binds an unaligned texel buffer and has no evidence tying it to this crash, so it is deliberately left out and tracked as a follow-up.
3. Not supported by the evidence: invalid/null/destroyed handles (non-null, accepted by the validation layer), the number of writes in the batch (the single write crashes alone), lifetime/synchronization (no hazard VUIDs).

## Fix
- `needsIndividualTexelBufferBindingWrites` (new `QuirkManager` flag, Qualcomm proprietary only): each `UniformTexelBuffer`/`StorageTexelBuffer` binding is written by its own `VkWriteDescriptorSet`, with the binding's own `descriptorCount`. Views, handles, offsets, ranges, formats and their order in the flat array are unchanged. Other drivers keep the single batched write.
- `gpu/interconnect/common/descriptor_write_policy.h`: `PlanDescriptorWrites`/`DescriptorWriteCount` decide the write layout and the number of writes allocated, used by `MakePipelineDescriptorInfo` and `Pipeline::SyncDescriptors` so the allocation and the emission cannot disagree. It is a host-testable header.
- Not done: no offset rounding, no handle substitution, no global waits, and the experimental ten-way split is not kept. `SyncDescriptorsQuickBind` already issues one write per binding, so it is unchanged.

## Diagnostics
Removed: `FE3H-DESC *` / `SPLIT_MODE` (`state_updater.h`), `FE3H-TEX-1D *` (`textures.cpp`), `FE3H-TEXEL viewReturned`. The 1D shader/2D view mismatch (`VUID-vkCmdDispatch-viewType-07752`, compute `cs_tex2_28`) is unrelated to this crash and remains **unresolved**; its probes are recoverable from PR #433.
Kept (Debug only, bounded to 32 lines): `TEXEL-ALIGN misaligned view creation ...`, so the next capture shows whether any misaligned texel view exists. The DevDebug Vulkan validation layer and synchronization validation stay enabled (`gpu.cpp`).

## Tests
`tests/fire_emblem_descriptors/` (run in CI before the build):
- `tests.cpp`: host test (ASan/UBSan) of the production planner, including the exact Fire Emblem layout (bindings 1–3, one element each: one write of count 3 when batched, three writes of count 1 when individual), arrays, empty layouts and the invariant that the planned count equals the allocated count and that all descriptors are covered once in order.
- `run.py`: wiring guards (quirk is Qualcomm-only, graphics path uses the planner for both texel types, sizing and emission agree), a check that texel buffer offsets are not rewritten, that the temporary instrumentation is gone, and that CI compiles only DevDebug with the validation layer.
Nothing in this environment can compile the Android target; compilation and the device run are covered by the CI build below.

## Device validation
1. Install the **ARM64 DevDebug Validation** APK from the PR's CI run, with the Qualcomm proprietary driver, same game version and settings as the supplied logs.
2. Expected if the diagnosis is right: no crash at the first texel buffer write with count > 1, and the log shows `Needs Individual Texel Buffer Binding Writes: true` in the device summary.
3. If it still crashes in `vkUpdateDescriptorSets`, the batched multi-binding write is excluded as the cause. Next, check the log for `TEXEL-ALIGN` lines (misalignment) and for the formats of the three views; do not widen the change without that evidence.
4. Confirm other games are unaffected: on Turnip/Mesa and on compute-only paths the writes are identical to before.
