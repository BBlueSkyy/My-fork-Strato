# SpongeBob: The Cosmic Shake — overlap readback diagnostic

This diagnostic branch is based on PR #400 at `1475633e58f9480411d0b1a3cde841f0b15e6138`. It does not change TextureManager ownership decisions, region transfer eligibility, Vulkan commands, the Free Guest Texture Memory policy or shader behavior. PR #400 itself stays untouched. No graphics fix is asserted.

## Evidence and question

The master footage shows large visual changes between launches, including a mostly correct title scene. PR #400 footage shows a more repeatable but still broken title scene (white/pink/black shapes). The question is whether incompatible texture overlaps are materialized from guest bytes while a prior GPU producer's readback is skipped, or whether incorrect output originates elsewhere.

## Instrumentation

Only `app/src/main/cpp/skyline/gpu/texture_manager.cpp` is modified.

- `TextureManager::SynchronizeGuestOverlaps` checks the existing source under its existing resource/state locks. For a `GpuDirty` source whose `gpuWriteSerial > gpuReadbackSerial`, it compares the serial after the *unchanged* call to `SynchronizeGuest(false, true)`.
- `outcome=no_readback` means the serial did not catch up with the recorded write in that overlap lookup. It is **not proof of stale data or a bug**: a superseded alias or region-owned resource can legitimately avoid whole-image readback. Fields `stale`, `readonly`, `regional`, `undefined`, `format_mismatch`, `was_freed`, `dirty_before/after` and dimensions help distinguish cases.
- `outcome=readback_done` means the recorded serial caught up. It does **not** prove the bytes are correct.
- `TextureManager::FindOrCreate` logs `region outcome=accepted|rejected` for new textures that have actual overlapping sources, including geometry, mip levels, layers and tile mode. Rejection reasons are not individually resolved yet.
- `[SB_TEXDIAG]` events are sampled: first 24 `no_readback`, first 8 `readback_done`, first 24 rejected and first 8 accepted regional registrations; thereafter events 32, 64, 128, etc. Global atomic counters continue counting all qualifying events for the lifetime of the process. No per-frame texture cache scan was added. Logging remains asynchronous and bounded, although it can still perturb timing.

## Device test

Use the diagnostics APK, same Vulkan driver, game update, shader cache policy, and settings across tests:

1. Fully close the emulator and relaunch SpongeBob with **Free Guest Texture Memory OFF**. Stop at the first title screen, capture screenshot and complete `emulation.log`.
2. Fully close and relaunch the same APK with Free Guest OFF. Capture the second title screen and log, even if both look equally corrupt.
3. Record whether each boot was visually correct/corrupt. Do not confuse repeated identical corruption with correct texture contents.
4. Search the log for `SB_TEXDIAG` and compare `no_readback` counts and the `undefined`/`format_mismatch` fields, plus accepted/rejected regional counts. Pointer addresses are only within-process IDs; dimensions/formats can be matched across boots.
5. Only after this baseline, optionally repeat ON with identical settings except Free Guest.

If no `no_readback` candidates appear, or all nonpublished sources are stale/read-only regional aliases, this diagnostic alone does not implicate TexMan; use a mini RenderDoc / GPU capture to check texture and buffer inputs before investigating shaders. If a live non-stale untracked producer repeatedly has `undefined=1` or `format_mismatch=1` with `no_readback`, collect its precise prior writer/consumer path before changing synchronization behavior.

## Validation boundary

This is log-only instrumentation. Static source inspection confirms that the original readback call and existing fallback/drain behavior remain in the same order. An Android compile and real-device log comparison are required. Do not merge the diagnostic into PR #400 without evidence.
