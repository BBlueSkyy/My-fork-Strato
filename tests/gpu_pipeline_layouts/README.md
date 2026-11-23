# Vulkan pipeline layouts

Native tests of the production device-local layout cache. No Android build is
needed or invoked. Use Clang, the pinned Vulkan-Hpp/headers and thread-pool
submodules, Boost headers, a Vulkan loader/ICD, Khronos validation and SPIR-V Tools.

```sh
git submodule update --init app/libraries/vkhpp app/libraries/thread-pool
git -C app/libraries/vkhpp submodule update --init Vulkan-Headers
CXX=clang++ python3 tests/gpu_pipeline_layouts/run.py
```

Set `BOOST_INCLUDE_DIR`, `VULKAN_HPP_INCLUDE_DIR`, `VULKAN_HEADERS_INCLUDE_DIR` and
`VULKAN_LOADER_LIBRARY` when dependencies are not in their default locations.
`VK_ICD_FILENAMES` and `VK_LAYER_PATH` select the ICD and validation layer. The
graphics production code uses Clang's supported designated-initializer ordering;
the integration suite requires Clang. Benchmarks may also compile with GCC.

The runner compiles actual graphics ownership/assembly/async bodies, actual compute
creation/ownership and the full DescriptorAllocator implementation. Platform state,
the JVM callback and GPU feature selection are small service facades; this is not
a complete Android/channel/Maxwell execution. Shaders are original small SPIR-V
test programs, assembled and validated before use. Coverage includes:

- Structural binding/count/type/stage/flags and push-constant identity, ordering
  normalization, push/ordinary isolation, and separate descriptor sharing when
  only push constants differ.
- Safe bypass for immutable samplers and descriptor/pipeline extension chains.
  Unknown flags and pipeline pNext forwarding are checked by injected creation
  errors before the driver, without issuing an invalid Vulkan operation.
- Simultaneous creation, hash-table rehashes, failure/retry, and ownership retained
  by a pending task after cache teardown.
- Real ordinary and push-descriptor compute dispatches, distinct shader results and
  integer bits, graphics async compilation, pipeline switches, push constants and
  exact readback pixels.
- Actual descriptor-slot exclusion while active and reuse after completion.

ASan/UBSan are enabled. LeakSanitizer is disabled by default because it cannot run
under the executor's ptrace configuration; this is not a leak audit. Layout create
and destroy counts must still balance. VVL synchronization validation is enabled
and any validation error fails the suite. Native Vulkan success does not establish
Adreno/Android gameplay, frametime or thermal behavior.

## Optional benchmark

```sh
CXX=clang++ python3 tests/gpu_pipeline_layouts/run.py --benchmark
CXX=clang++ python3 tests/gpu_pipeline_layouts/run.py --benchmark --unique-control
```

The baseline uses the same two create infos and inline RAII ownership as master
`e8e38237`, without adding shared-pointer or key allocation to its cost. The candidate
uses the actual production cache. Each epoch replays the fixture once with a fresh
cache, retaining all results through its end. It does not repeat requests into a
permanently warm cache to inflate the hit rate. Eleven paired rounds alternate
order, 300 epochs per variant per round, after warmup. Linux affinity pins the
benchmark to the first allowed CPU. Timing excludes destruction and uses a device
without validation, dispatch hooks or allocation counters. Object and heap
measurements use a separate instrumented device, outside the timed pass.

`heap_delta_bytes` is a glibc `mallinfo2().uordblks` snapshot relative to reserved
owner vectors; it includes cache metadata and driver heap allocations visible to
glibc, varies with allocator bins, and is not GPU or Android process memory.
Driver allocation callbacks may report zero when the driver does not use them.
The unique control intentionally exposes cache-miss CPU/metadata overhead.

`capture-layouts.txt` contains only 66 layout create-info definitions recovered
offline from the user's historical SpongeBob RenderDoc frame638. There are 30
unique descriptor interfaces and 30 unique pipeline interfaces. It is not a trace
from the current master and is not a full game's pipeline census. The current
uncached creation sites still duplicate objects for these same valid definitions.
No shaders, game content or capture binary is committed. Its checksum and the
offline extraction schema are recorded in `capture-summary.json`.

To reproduce the metadata with the original capture (system `liblz4.so.1`):

```sh
python3 tests/gpu_pipeline_layouts/extract_capture.py strato-frame_frame638.rdc --output /tmp/layout-capture
cmp /tmp/layout-capture/capture-layouts.txt tests/gpu_pipeline_layouts/capture-layouts.txt
```

Full findings, paired results and limitations are in
`docs/performance/shared-vulkan-layouts-20261123/README.md`.
