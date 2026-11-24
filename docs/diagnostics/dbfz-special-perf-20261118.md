# DBFZ special-attack performance diagnostic (2026-11-18)

Branch: `diagnostics/dbfz-special-perf-20261118` (Draft PR; no merge).
This is **measurement only**. It does not modify shader, texture, GC, scheduler,
submission ordering or the number of Vulkan synchronizations.

## Build switch

`STRATO_DBFZ_PERF_DIAGNOSTICS` defaults to **ON on this temporary branch**
so the regular PR ARM64 Release build emits aggregate `[DBFZ-PERF]` logs.
Use `-DSTRATO_DBFZ_PERF_DIAGNOSTICS=OFF` in a clean CMake configuration to
build the exact same code without the probes for an A/B performance comparison.
Do not merge this branch without first disabling/removing the diagnostics.

The probes currently collect for any running title on this *diagnostic* build.
The log prefix names the target experiment, not a title-ID filter.

## What is sampled

Once per two seconds of real wall-clock time, the presentation thread emits two
summary lines in `emulation.log`:

- Frame presentation delta: average/max ms and number of frames above 22/33 ms.
  This is based on the same presentation timestamps as the Strato FPS overlay.
- Independent host-wall intervals between reported frames (not GPU timestamps).
- Graphics pipeline variant cache misses; number, total and max elapsed CPU
  times for asynchronous graphics-pipeline builds; duration spent requesting
  those builds; cumulative and worst `shared_future::get()` durations when
  recording pipelines into command buffers.
- Command record time, waits for free command slots, waits for prior slot
  completion fences, and separate presentation-path timing for guest fence,
  texture alias reconciliation, swapchain fence, image acquire, host sync,
  copy submission and queue presentation.

All hot-path counters are lock-free, with relaxed atomics and no per-event
logging. Timing scopes do not add GPU commands, force any wait or call
`vkDeviceWaitIdle`. Durations are **CPU wall-clock time spent in each call**;
they are not GPU execution duration, and totals from different threads may
overlap. Presentation copy times measure host-side submission, not when GPU
finishes executing the copy. Pipeline-future get totals can include already
ready futures; check their maximum latency as well as total time.

## Reproduction on Moto G100

1. Use the same Strato settings, driver and game state across trials. Set
   `Free Guest Texture Memory` as in the normal reference test; note its value.
2. Launch DBFZ and let initial shaders/caches stabilize. Fight normally for
   20 s (baseline).
3. Perform a special attack; pause normal attacks for ~20 s after its animation.
4. Repeat **the same** special, then another special with larger effects; note
   whether the first invocation differs from repeats.
5. Repeat the entire scenario with this CMake switch OFF (or with the same
   uninstrumented master baseline). Keep both `emulation.log` files.

For analysis, extract `[DBFZ-PERF]` lines and align them with the approximate
attack time from the video. A peak in `compile_sum/max_ms`, `pipelines_new`
and `future_get_sum/max_ms` points to shader/pipeline creation or waiting.
A peak in `cmd_fence_wait` or `present_*_fence` suggests synchronization
stalls; a peak in `present_acquire` could be swapchain backpressure or frame
pacing. If frame intervals increase but the recorded CPU paths remain low,
investigate GPU duration with nonblocking Vulkan timestamps in a **separate**
step. None of these observations alone proves causality.

## Safety / limitations

- Probe overhead is expected to be small but unmeasured on the Moto G100:
  compare ON/OFF before relying on the numbers.
- No shader/pipeline/texture cache clearing, retention or invalidation is done.
- No true GPU time query is issued: command submission and GPU execution are
  distinct, and invasive timestamps are intentionally deferred.
- No APK/device test or frame-by-frame real-world validation is claimed by
  this source-only change. GitHub Actions compilation is a separate gate.
