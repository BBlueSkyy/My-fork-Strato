# Cosmic Shake — Free Guest Texture Memory A/B (diagnostic branch)

## Scope and status
This branch contains **temporary observational instrumentation**, not a graphical fix.
It is based on master at `8bcaee0be950b44234cdd2767a11578c74db2602`; it does not modify PR #400, #460 or #476. Do not merge the instrumented code into master.

Observed on Moto G100 / Adreno 650: the Cosmic Shake title screen varies from almost correct to extremely corrupted across boots in Strato. Enabling **Free Guest Texture Memory** makes the appearance more consistent, but does not establish the underlying cause. Eden consistently displays the menu correctly even if the same option is toggled.

## What is measured
Events are emitted from `texture.cpp` as `[GPU-MEMAB]` records:

| Event | Meaning |
|---|---|
| `generic-accepted-host`, `generic-accepted-inline` | Generic attachment requested GPU dirty ownership and the policy allows it, absent an explicit RT/storage writer |
| `generic-suppressed-host`, `generic-suppressed-inline` | The same request was suppressed by Free Guest OFF when no explicit GPU writer was registered |
| `free-applied` | FreeGuest actually freed guest backing |
| `free-guarded`, `free-readonly` | FreeGuest did not free backing because one of its guards blocked it |
| `explicit-gpu-writer` | A real GPU write path called RecordGpuWrite |
| `guest-readback-freed` | A guest readback was entered while backing was marked freed |
| `guest-readback-complete` | The GPU-to-guest copy completed and readback serial was published |
| `guest-readback-unavailable` | Existing code skipped readback due to undefined layout or incompatible format |
| `alias-implicit-rejected`, `alias-implicit-rejected-freed` | An implicit requested alias layout failed eligibility; the second label specifically confirms `memoryFreed=true` |
| `alias-writer-restore-attempt`, `alias-writer-restore-failed` | A freed alias writer entered the restore path (attempt, not proven success), or restoration failed |
| `alias-stale-download` | Reconciliation requested an authoritative writer readback |

Each record includes **option ON/OFF, event category, per-category counter, texture pointer within one run, dimensions, mip count, Vulkan host/guest formats**.

This enables comparisons by event category and texture properties instead of Vulkan handles or draw/frame numbers that vary across boots. **Never correlate raw texture pointers across separate process launches**.

Events count with relaxed atomics; only occurrences 1–4 and later powers of two through 65,536 produce a log line. Counters are per process, **not** per texture; `count` records are sampling markers, not complete per-resource inventories. Log instrumentation may affect timings; avoid deriving FPS claims from this build.

## A/B capture protocol

1. Use exactly the same Turnip driver and other graphics settings for both groups. Start with the initial title/menu screen, without entering gameplay.
2. Run 8–10 **full game boots** with **Free Guest OFF**, exiting the game completely between boots.
3. Run 8–10 full game boots with **Free Guest ON**, also exiting fully each time. Prefer an alternating OFF/ON order in another run if thermals/cache state could bias the result.
4. For each boot, record `OFF/ON | normal / mildly corrupted / severely corrupted | time to menu | APK commit`. Save screenshot and export the full emulation log for representative **good** and **bad** boots in each setting.
5. Compare `[GPU-MEMAB]` event counts, format/dimension signatures and first occurrence ordering. First ask whether ON/OFF changes generic dirty tracking on resources correlated with the bad menu. Next ask whether freed backing, alias readback, or an unsupported readback path is involved.
6. If a discrepancy is identified, add a **narrow second-stage trace for that resource** (TIC / guest mapping or shader hash) instead of reverting to general Mini-RenderDoc capture.

Example filter on a saved log: `grep -F '[GPU-MEMAB]' emulation.log`

## Existing open questions, not assumed causes

- `FreeGuest()` affects actual guest memory lifetime; `SynchronizeHost()` and `SynchronizeHostInline()` also use the toggle to influence generic `gpuDirty` ownership. The diagnostic separates these mechanisms.
- `RegisterAliases(memoryFreed=true)` with an implicit requested layout checks `CanReconcileGuestLayout` before the explicit restore path. This **may** reject an otherwise recoverable writer; tests must cover both recoverable and nonrecoverable cases before changing it.
- `pendingGpuWriteTag` is not present in this master texture implementation; it belongs to the separate #460 line of investigation. Do not silently port its logic.
- The texel-buffer fix in PR #476 passed targeted Vulkan tests but did not remove the corruption on device. This branch intentionally does not change shader compilation or texel buffer handling.
- The title screen can be correct while later gameplay corrupts in Eden. Do not assume both symptoms have one cause.

## Validation and cleanup

- Verify the Android build compiles; exercise at least one good and bad boot with logs exported.
- Do not ship this instrumentation as a general performance optimization or memory fix.
- Once a cause is confirmed by a reproducing test, implement it in a **different clean branch** without the diagnostics, test both Free Guest settings, and preserve regressions of MK8, SMO, BOTW and RE5.
- This diagnostic PR stays Draft and must not be merged.
