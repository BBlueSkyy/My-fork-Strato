# Descriptor compatibility CPU benchmark

Run the semantic regression tests with ASan/UBSan:

```sh
python3 tests/gpu_tic_validation/run.py --probe-cache
python3 tests/gpu_tic_validation/run.py
```

The optional benchmark compares the full production compatibility function and
PrepareDraw descriptor lambda with an explicit Git baseline:

```sh
python3 tests/gpu_binding_match/benchmark.py --baseline 8bcaee0be950b44234cdd2767a11578c74db2602 --iterations 1500000 --rounds 11 --output /tmp/binding.json
```

Initialize `app/libraries/robin-map` at the pinned revision. Requires Linux CPU
affinity, C++20, GCC/Clang and Python 3. Uses production specialization and map
code with resource/descriptor facades. It pins the process to one available CPU,
alternates variant order, warms both and reports every round plus medians. The
cold case includes clearing the map. It is not a Vulkan, Android, FPS, complete
PrepareDraw or allocation-count benchmark. No CI timing threshold is imposed.

The [audit](../../docs/performance/peanuts-v0.1.6-1/README.md) records retained and
rejected experiments, raw results, limitations and remaining work.
