# Swapchain retirement host regressions

Run `python3 tests/gpu_presentation/run.py`. The runner extracts the complete
production `UpdateSurface`, `UpdateSwapchain` and `DestroySwapchain` methods.
Vulkan structs/enums come from pinned Vulkan-Hpp/Headers. JNI, native-window,
queue, resource-allocation and settings boundaries are host facades; this is
not a real WSI/driver or Android build test.

The queue facade holds pending work behind a gate and records destruction
before release. Tests cover detach/replacement/resize, failure preservation,
external queue synchronization, no double drain, and first surface creation.
No test API or synchronization hook is added to production.

```sh
STRESS_RUNS=25 python3 tests/gpu_presentation/run.py
ASAN_OPTIONS=detect_leaks=0 SANITIZE=address,undefined python3 tests/gpu_presentation/run.py
SANITIZE=thread python3 tests/gpu_presentation/run.py
PRESENTATION_BASELINE_REF=860c0cd1 python3 tests/gpu_presentation/run.py
```

The baseline command must fail pending-write destruction assertions. Test
deadlines observe progress and never release the gate automatically.
See [audit and limits](../../docs/diagnostics/mk11-pr362-20261116-audit.md).
