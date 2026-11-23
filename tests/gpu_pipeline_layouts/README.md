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
- Simultaneous creation, hash-table rehashes, failure/retry, 1,024 interfaces grown
  concurrently by eight workers, and primary/secondary ownership after teardown.
- A deferred real compute compilation after cache teardown and weak-root release
  checks to detect ownership cycles.
- Actual helper blit/clear handle consumer statements extracted and compiled;
  platform/push-data types are explicit facades, not the full helper translation unit.
- Real ordinary and push-descriptor compute dispatches, distinct shader results and
  integer bits, graphics async compilation, pipeline switches, push constants and
  exact readback pixels.
- Actual descriptor-slot exclusion while active and reuse after completion, plus
  graphics/compute sharing of one set with different push-constant interfaces.

ASan/UBSan are enabled. LeakSanitizer is disabled by default because it cannot run
under the executor's ptrace configuration; this is not a leak audit. Layout create
and destroy counts must still balance. VVL synchronization validation is enabled
and any validation error fails the suite. Native Vulkan success does not establish
Adreno/Android gameplay, frametime or thermal behavior.

## Optional comparative benchmark

```sh
CXX=clang++ python3 tests/gpu_pipeline_layouts/run.py --benchmark
CXX=clang++ python3 tests/gpu_pipeline_layouts/run.py --benchmark --unique-control
CXX=clang++ python3 tests/gpu_pipeline_layouts/run.py --benchmark --review-ref e8e6b089 --suite --build-dir /tmp/layout-review > /tmp/rounds.jsonl
```

`--review-ref` reads the original cache from a local git commit, without checking
out or modifying that revision. It generates a reference namespace and a separate
single-thread no-lock diagnostic variant. The reference's private visibility is
opened solely to measure its existing hash/key helpers. No production policy or
structural comparison is disabled. The no-lock variant is never used in production.
Do not use it on multiple threads.

The baseline uses the same two create infos and inline RAII ownership as master
`e8e38237`, without adding shared-pointer/key allocations to its cost. Candidates
execute the actual cache, with full structural lookups, not pre-indexed recipe IDs.
Each epoch retains the results of one sequence until its end, using a fresh cache.
Twelve rounds rotate the variant order, 300 epochs each after 25 warmups. Affinity
pins the benchmark to the first allowed CPU. Workloads execute sequentially.
Do not run another benchmark or compiler on that CPU during measurement.

The timed executable has no sanitizers, VVL, Vulkan hooks, new overrides or counters.
Creation timing excludes destruction. Lifecycle timing includes cache/result
teardown and the equally reserved owner vectors. A **separate diagnostic executable**
intercepts object creation and ordinary C++ new calls. C++ new counts are metadata
allocation observations, not driver/GPU allocation counts. Its memory samples use
glibc `mallinfo2().uordblks`; normal glibc tcache may obscure free cached chunks.
For a cleaner comparison run the already compiled diagnostic in a fresh process:

```sh
GLIBC_TUNABLES=glibc.malloc.tcache_count=0 /tmp/layout-review/diagnostic /tmp/layout-review/workloads/unique.txt
```

Repeat that command for `capture-layouts.txt` and each generated fixture, recording
the case name. This changes only diagnostic allocator configuration, **not timing**.
The published `heap-no-tcache.jsonl` holds five samples per case with that setting;
`review-rounds.jsonl` also retains normal-tcache diagnostic samples for transparency.
Neither heap snapshot is GPU memory, RSS or Android RAM. Zero driver allocation
callbacks do not mean zero memory usage.

`--suite` generates deterministic dense controls through `make_workloads.py`:

| Case | Requests | Unique DSL / PL | Purpose |
| --- | ---: | ---: | --- |
| capture | 66 | 30 / 30 | Historical layout metadata |
| unique | 128 | 128 / 128 | Original cold regression |
| repeated | 128 | 8 / 8 | High interface reuse |
| mixed | 128 | 64 / 64 | 64 misses followed by shuffled repeats |
| push_variants | 128 | 8 / 128 | Reuse descriptors without reusing pipeline layouts |
| growth | 2,048 | 2,048 / 2,048 | Table growth, no hits |

The controls expose regressions rather than hiding them in repeated frames.
`IsolatedCosts` times reference descriptor/range hashing, owned key construction
and uncontended locking separately. These loops do not sum to an exact cost profile.
The no-lock comparison provides an additional ablation in the full Acquire path.
`summarize.py` combines paired rounds with separate heap records; committed exact
summaries and checksums are in the report's `validation.json`.

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


## Miss-cost follow-up from c0a47a2c

The current primary index finalizes only its table hash; structural and secondary
hashes remain unchanged. To reproduce the dedicated stages together with the full
paired workloads:

```sh
CXX=clang++ python3 tests/gpu_pipeline_layouts/run.py --benchmark --review-ref c0a47a2c --suite --miss-profile --build-dir /tmp/layout-misses > /tmp/miss-rounds.jsonl
```

`miss_profile.cpp` uses exact reference/current headers with visibility opened for
diagnostics. It measures owned key construction, hashing/finalization, equal and
different comparisons, sized allocation controls, copied/moved table insertion,
lookup, locking and complete shared native/Entry construction. It includes an empty
loop control; the stages are not an additive decomposition. Real Vulkan entries are
precreated outside insertion/lookup timing. Counts are collected separately after
timing. Profiling executes only the unique workload and rejects duplicate descriptor
interfaces. The older four-stage reference loop also accepts c0's explicit key ctor.

The native suite additionally forces 64 primary hash collisions with a generated
copy of the actual cache, retaining full structural equality. A retention
observation uses the actual DescriptorAllocator and a still-live pool; it never
reuses a destroyed handle. It records whether pooled slots retain layout owners,
without asserting that a particular driver recycles handles.

The [follow-up report](../../docs/performance/shared-vulkan-layouts-20261123/miss-cost-c0a47a2c/README.md)
contains final comparative results and the safe-growth policy evaluation. It adds
no production eviction/cap. Previous reports and checksums remain snapshots of
their named commits, rather than claiming to describe the current source.
