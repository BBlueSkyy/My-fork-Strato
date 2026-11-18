# MK11: Turnip DEVICE_LOST after returning to character/arena selection (2026-11-17)

## Reproduction evidence
- User supplied a complete 1,743-line emulation.log from Moto G100 / Adreno 650 / Android 12.
- Game: Mortal Kombat 11, title ID `0100F2200C984000`, version 0, no update loaded in that session.
- Vulkan driver: Turnip Adreno 650, Vulkan 1.3.330 (Mesa 25.99.99).
- On leaving a fight and loading the UI/character selection, the game logs asynchronous package loads.
- At **96.021178 seconds**, GPFIFO reports `Vk../src/freedreno/vulkan/tu_descriptor_set.cc:577: ... VK_ERROR_OUT_OF_POOL_MEMORY`.
- Between 97 and 99 seconds, recurring out-of-range TSC indices 1836, 3505, 1772, and 2993 are reported.
- At **100.929905 seconds**, Turnip reports `GPU faulted or hung (VK_ERROR_DEVICE_LOST)` from `tu_knl_kgsl.cc:1663`.
- The log does **not** contain validation-layer VUIDs, a native GPU backtrace, or enough evidence to assign causality to pool exhaustion, TSC indices, shaders, GC, or physical device memory pressure.

## Existing code checked
- `DescriptorAllocator::AllocateVkDescriptorSet` explicitly accepts `VK_ERROR_OUT_OF_POOL_MEMORY` and `VK_ERROR_FRAGMENTED_POOL`; `AllocateSet` can reset an unowned pool or create a larger pool. Therefore a single driver warning need not be terminal.
- `Samplers::GetSampler` warns on an out-of-range pool index and returns null. The already present fallback to `GetNullSampler` for combined-image sampler bindings must not be removed without contrary evidence.
- PR #463 currently implements conservative texture GC; this diagnostic branch is created **from #463 HEAD `43ead4b3`**, without modifying #463.
- These observations do not identify the first invalid GPU command. No functional GPU fix is claimed.

## Diagnostic implementation
- Reuse the proven configuration from Strato PR #433: **Khronos Vulkan Validation Layer 1.4.363.1** plus **Synchronization Validation**, enabled only for Android **DevDebug**.
- CI obtains the layer from the exact Khronos Android archive with a pinned SHA-256 and packages `libVkLayer_khronos_validation.so` under `app/src/debug/jniLibs/arm64-v8a`; the APK verifier checks it.
- Preserve all existing host GPU checks; CI assembles only a dedicated **ARM64 DevDebug Validation** APK.
- Add bounded, Debug-only `MK11-DESC pool-created` and `MK11-DESC allocation-failed` messages. These distinguish successful pool expansion from persistent exhaustion, without changing descriptor sizes, pool lifetime, synchronization, shader compilation, or Vulkan submission.
- Diagnostic-only code is guarded by `#ifndef NDEBUG`; regular release behavior is unchanged.

## Device validation and next decision
1. On Moto G100, use the same Turnip 1.3.330 and reproducible sequence (fight -> character select -> choose another arena). Keep settings the same as the original log.
2. Install the CI artifact `strato-pr<PR>-arm64-debug-validation.apk`. Verify the new log contains `Vulkan validation enabled: ... synchronization=true (DevDebug)`.
3. Capture the *entire* `emulation.log`, including the first relevant `VUID-` or `SYNC-` messages, `MK11-DESC` records, TSC warnings, and terminal `VK_ERROR_DEVICE_LOST`. Do not rely only on the final error.
4. Correlate validation messages with pool generation, descriptor failures/retries and the UI transition. Fix the **first proven invalid operation** in a minimal separate commit; rerun Vulkan validation/regressions before preparing a normal DevRelease.
5. If DevDebug does not crash, do **not** conclude the release problem is fixed: the layer changes timing and memory pressure. Cross-check the same sequence with the uninstrumented build. If VUIDs do not explain the device fault, inspect GPU lifetime/sync and Turnip device-fault evidence in a narrower follow-up.

## Scope
- No merge, no changes to the source PR #463, no FE3H-specific probes, no shader compiler changes.
- No forced GPU idle, disabling synchronization, unbounded logging, or game-specific compatibility hacks.
- Diagnostic PR based on the unmerged #463 head; targeting master solely to run the existing PR Build trigger. **Do not merge this PR into master as-is.**
- Android compilation and gameplay verification are not available in the current environment; GitHub Actions and the on-device capture are the validation gates.
