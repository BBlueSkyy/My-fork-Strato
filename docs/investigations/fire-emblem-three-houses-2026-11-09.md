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
- Detailed records for first 256 updates; thereafter every 32nd update. A matching `AFTER` distinguishes a returned call from a fault within the driver. Sampling can miss the exact last call later in a long session; Vulkan VUID messages remain essential.
- Logs are **diagnostic only**. No descriptor rewriting, forced waits, resource lifetime changes, or GPU/compiler behavior change is included. The prior CI run compiled DevDebug but failed packaging verification because the layer had been installed outside an Android JNI source set; its path is corrected here.

## Device test
1. Download the **ARM64 DevDebug Validation** APK from this PR's CI, install it, and use the Qualcomm proprietary driver with the same game version/settings as the supplied log.
2. Launch Fire Emblem: Three Houses once, stopping at the crash; save the full `emulation.log`, including the `Vulkan validation enabled` line, any `VkValidation` VUIDs, and `FE3H-DESC` lines.
3. If the same crash is reproduced, repeat with a Turnip driver **without clearing the shader cache between tests** when possible and identify the driver version in each log. The two logs must be labeled by driver.
4. If DevDebug does not reproduce, do not assume that a debug-only success proves a correction. A release comparison will be considered only after collecting device evidence.

If the debug build cannot load the layer, record its startup error rather than interpreting the failure as a game regression.

## Next step
Correlate the last descriptor update and the **first relevant validation VUID** with `state_updater.h`, the texture/buffer descriptor producers, and `DescriptorAllocator::ActiveDescriptorSet` lifetime. Only then introduce a minimal, independently testable fix. Keep this PR Draft and do not merge until the device evidence and regression tests support the fix.
