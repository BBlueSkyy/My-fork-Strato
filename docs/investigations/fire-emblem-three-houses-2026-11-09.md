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

This establishes a **direct association** between two invalid view creations and the exact native descriptor update where Qualcomm crashes. It does not yet prove the invalid views caused the driver segfault, which is why the next experiment isolates the ten writes. The Qualcomm Vulkan 1.1.276 extension list in the captured device log does not contain `VK_EXT_texel_buffer_alignment`.

### Next DevDebug-only A/B
For the narrowly matched graphics call (10 writes, binding 1 UniformTexelBuffer count 3), issue the **same** ten Vulkan writes in ten sequential calls, logging `FE3H-DESC WRITE_ITEM_BEGIN/END` for each. All original handles, descriptor counts, layouts, offsets, and write order remain unchanged. If the crash occurs in item 1, it isolates the texel descriptor write. If the full update succeeds, investigate batch-sensitive driver behavior, but do **not** treat success as a Vulkan-correct fix. Never align the underlying offset by rounding without preserving shader texel indexing. DevRelease remains unaffected by the diagnostic experiment.

## Next step
Correlate the last descriptor update and the **first relevant validation VUID** with `state_updater.h`, the texture/buffer descriptor producers, and `DescriptorAllocator::ActiveDescriptorSet` lifetime. Only then introduce a minimal, independently testable fix. Keep this PR Draft and do not merge until the device evidence and regression tests support the fix.
