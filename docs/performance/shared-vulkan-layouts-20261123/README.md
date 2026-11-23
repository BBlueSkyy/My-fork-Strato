# Reuse structurally identical Vulkan layouts

Base: `e8e38237aa5993deeaaae9f01f39ea08500f18db` (`master`, rechecked before publication).
Branch: `perf/shared-vulkan-layouts-20261123`.
Reference: [PR #482](https://github.com/BBlueSkyy/My-fork-Strato/pull/482).

## Result

Implemented device-local sharing of descriptor-set and pipeline layouts after
confirming duplicate Vulkan creations in a historical user capture and measuring
the uncached creation cost. On a native replay of its **layout definitions**, each
kind of object decreased from **66 to 30**, with 36 cache hits. The measured heap
delta for these objects and cache metadata decreased from **64,928 to 33,728 bytes**.

The paired creation-only microbenchmark decreased from **14.26 to 13.27 microseconds
per 66-request sequence**, approximately 7%. This saves approximately **one
microsecond in this isolated host workload**, not 7% of frame time or game CPU time.
There is no measured FPS, Android RAM or Adreno frametime improvement.

The all-unique control is more expensive: **9.74 to 28.73 microseconds per 128
requests**, with **98,768 to 149,056 bytes** of measured heap delta. This is a real
cache-miss/metadata cost, not a universal performance win. Publication is Draft;
Android measurements and regression checks remain necessary before integration.

## Audit and duplication evidence

The existing pipeline cache/manager already reuses complete pipelines, retains
specialized variants, uses transition caches, preloads persistent bundles and
compiles graphics pipelines asynchronously. None of that was replaced. Descriptor
quick bind already reuses contents under specialization and coherence checks.

The missing reuse is between **different pipelines with the same Vulkan interface**:

- `GraphicsPipelineAssembler::AssemblePipelineAsync` created a descriptor-set
  layout and a pipeline layout on every new graphics pipeline.
- Compute `MakeCompiledPipeline` did the same.
- `DescriptorAllocator::AllocateSet` partitions slots by the native layout handle.
  Its header explicitly asks callers to reuse equivalent layout objects. Pool
  reset/recycling already exists, but different handles partition available slots.
- No content-keyed layout interning existed at these two creation sites.

The prior #482 audit identified this opportunity without measuring it. Its separate
quick-bind predicate change was not imported into this branch.

An offline read of `strato-frame_frame638.rdc` found 66 `vkCreateDescriptorSetLayout`
and 66 `vkCreatePipelineLayout` chunks, with 30 unique definitions of each. Groups
of up to six identical interfaces have distinct captured handles. All use one set,
no extension chains and no immutable samplers. An additional small frame329 capture
also contained a duplicated interface (three pairs, two unique); it is not used to
weight the benchmark.

The frame638 capture is historical SpongeBob material, **not a run of the current
master**. Therefore this establishes observed duplicate interfaces, not the present
game's creation frequency after every intervening shader/resource fix. The current
master still creates separate objects for these same valid inputs; the benchmark
replays those definitions through the baseline construction sequence and actual
new cache. It does not replay the game or count all pipelines of a play session.

Source capture SHA-256:
`d7f5cb33f83b7284deba1c51b247a419cd2a4da6c0204dbd125903f1d5974e11`.
Its 144,988,840 bytes contain 706,344,000 decompressed bytes and 4,387 parsed chunks.
The strict extraction script checks section/chunk boundaries, serialization types,
array lengths, absent extensions/samplers and exact consumption of creation chunks.
Only layout metadata is committed; no shaders, game resources or RDC binary.

See [capture summary](../../../tests/gpu_pipeline_layouts/capture-summary.json),
[66 input definitions](../../../tests/gpu_pipeline_layouts/capture-layouts.txt) and
[reproduction script](../../../tests/gpu_pipeline_layouts/extract_capture.py).

## Implementation and correctness boundaries

[`PipelineLayoutCache`](../../../app/src/main/cpp/skyline/gpu/pipeline_layout_cache.h)
is owned by `GPU`, shared by graphics and compute. It has two levels:

1. Descriptor interfaces: binding number, descriptor type, count, stage masks and
   every descriptor-layout flag. The binding order is normalized. Complete value
   equality resolves hash collisions; the hash alone never establishes equivalence.
2. Pipeline interfaces: the canonical descriptor interface, every pipeline-layout
   flag, and complete ordered push-constant ranges (stage mask, offset, size).
   Different push ranges may still share the descriptor-set layout.

Current builders emit ascending bindings. A borrowed, heterogeneous lookup avoids
allocating a temporary key on hits. It hashes the existing binding records; new
owned keys are allocated only on misses. A first implementation that allocated
both keys on every lookup increased CPU cost in the capture workload and was
discarded before the final measurements. The published results use the complete
allocation-free-hit implementation, not a pre-indexed recipe-ID shortcut.

Any descriptor/pipeline `pNext`, or any immutable-sampler pointer, uses a fresh,
uncached pair and forwards the original creation data. This is an intentional
admission boundary, not missing fields silently excluded from a key. Those paths
need extension-specific serialization or sampler ownership to admit them safely;
the current builders use neither and no benefit justified extending their scope.
Immutable sampler handle recycling consequently cannot cause a false cache hit.

One mutex covers lookup/publication and cold creation, preventing concurrent
duplicate objects. Exceptions publish no partially created pipeline layout. A set
layout successfully created before a pipeline-layout failure remains reusable;
retry does not return a failed entry.

The immutable cache is per logical device and is invalidated by destroying that
GPU/device scope. Canonical handles are strongly retained until teardown. Inactive
descriptor-set buckets use those handles as keys, so weak retention/early eviction
could let a recycled native handle name an incompatible bucket. The new GPU member
is declared before `DescriptorAllocator`, ensuring its layouts outlive the pools.
Pipeline-map keys refer to stable descriptor-map nodes; rehash preserves their
addresses, and pipeline entries are destroyed before descriptor entries.

Compiled graphics/compute pipelines own shared layout entries and expose borrowed
native handles to their existing descriptor/update paths. Graphics compilation
tasks also capture an owner. Destruction order releases pipeline layouts before
their descriptor-layout owners. The Vulkan device must outlive those owners, as
with the previous RAII objects; this is not a cross-device cache.

There is no new Vulkan extension requirement. Layout flags still come from
`CanUsePushDescriptors` and the original `noPushDescriptors` decision. Persistent
pipeline/shader cache versions and keys are unchanged. Equal layout objects do not
mean equal descriptor contents: specialization, Float/UINT/SINT selection, handle
snapshots, binding matches, quick-bind eligibility and resource invalidation retain
their existing checks. The Maxwell/compute descriptor call-site edits only unwrap
the now-borrowed handles instead of the previous individual RAII objects.

Texture aliases, mip promotion, Free Guest, GC, barriers, fences and queue submission
code are unchanged. The accepted preexisting Free Guest backing limitation recorded
with #484 is not fixed or declared green by this work.

## Measurements

Host: x86-64, Clang 19.1.7, C++20 `-O3 -DNDEBUG`, llvmpipe LLVM 19.1.7. Native
Vulkan-Hpp is pinned to `80dada6a7b455cf18c39788d70aa7711323ea977`, with its pinned
Vulkan headers. Timing and memory are separate measurements:

- Eleven paired rounds, alternating variant order, 300 fresh-cache epochs per
  variant per round, after 25 warmups. Linux affinity pins to allowed CPU 0.
- Every epoch replays one sequence and keeps its objects alive until its end. The
  cache is recreated each epoch; repeated frames are not fed into an already-hot
  cache to manufacture a higher hit rate.
- Timing includes construction/lookup/ownership, excludes destruction, and uses
  an uninstrumented device without validation or injected allocation callbacks.
  Baseline ownership is inline RAII, as in master; it is not charged an extra
  shared-pointer/key allocation to favor the candidate.
- A separate device intercepts Vulkan creation calls to count actual API objects.
  Five heap snapshots use glibc `mallinfo2().uordblks`, relative to equally reserved
  owner vectors. Cache keys/nodes and driver allocations visible to glibc are
  included. Allocator bins affect the samples; this is not GPU allocation size,
  process RSS, Android RAM or total pipeline/shader memory. Driver allocation
  callbacks reported zero in this ICD and are not used as a memory estimate.
- The final capture and unique-control runs were executed sequentially. Functional
  sanitizer/VVL runs are not included in the timings.

| Historical capture definitions, 66 requests | Master construction | Cache |
| --- | ---: | ---: |
| Descriptor-set layouts created | 66 | 30 |
| Pipeline layouts created | 66 | 30 |
| Pipeline cache hits | 0 | 36 (54.5%) |
| Descriptor-layout reuses | 0 | 36 |
| Median creation time for the sequence | 14.26 µs | 13.27 µs |
| Minimum–maximum round averages | 13.74–14.85 µs | 12.85–14.58 µs |
| Median heap delta | 64,928 B | 33,728 B |

The primary demonstrated gain is fewer layout objects and less retained heap in
this repeated-interface workload. The time difference is small in absolute terms.

| All-unique synthetic control, 128 requests | Master construction | Cache |
| --- | ---: | ---: |
| Descriptor-set layouts / pipeline layouts created | 128 / 128 | 128 / 128 |
| Cache hits | 0 | 0 |
| Median creation time for the sequence | 9.74 µs | 28.73 µs |
| Median heap delta | 98,768 B | 149,056 B |

All-unique input pays approximately 18.99 µs extra over 128 misses (0.148 µs per
request) and 50,288 B extra retained heap. The control deliberately uses small
valid interfaces; its relative penalty must not be hidden by reporting only a
duplicate-heavy workload. Actual Android reuse rates/driver costs decide the
tradeoff. No whole-pipeline compilation benchmark establishes that it is negligible
in a real game's frame budget.

Raw paired rounds: [capture](capture-rounds.json), [unique control](unique_control-rounds.json).
Metadata, exact medians and source checksums: [validation.json](validation.json).

## Validation performed

| Check | Result |
| --- | --- |
| Production cache + native integration | 259 checks passed with ASan/UBSan |
| Khronos VVL 1.4.309, synchronization validation enabled | Zero errors |
| SPIR-V assembly/validation for original test shaders | Passed |
| Graphics production description/async creation + ownership | Compiled and executed with real Vulkan |
| Compute production creation + ownership | Compiled; ordinary and push-descriptor dispatches executed |
| Production DescriptorAllocator | Active slots excluded; completed compatible slots reused |
| Distinct shader outputs / integer bits / graphics pixels / push constants | Exact comparisons passed |
| Concurrent first use, rehash, failure/retry, flags, extension/sampler bypass | Passed |
| Pending task after cache teardown and separate logical devices | Passed |
| Existing TIC executable suite | 1,186 checks, zero failures |
| Existing specialization/cache/descriptor probe | 129 checks, zero failures; Float/UINT/SINT variants preserved |
| Existing production preload lifetime test | Passed success, corruption and non-cache exception cases |
| Metadata extraction reproduces committed fixture | Byte-identical |
| `git diff --check` | Passed |

The integration harness compiles the actual changed graphics and compute creation
bodies and ownership structs, the production graphics description/assembly code,
and the full descriptor allocator. Platform services, feature selection and the
JVM callback are explicit facades. It does not build the complete Android GPU,
translate guest shaders or execute a Maxwell command stream. The device/GPU member
wiring is also reviewed in the production diff; it is not an Android link check.

LeakSanitizer was disabled because the executor uses ptrace; no leak audit is
claimed. ASan/UBSan and create/destroy balancing still run. The tests use no invalid
GPU commands. Future flags/unknown pipeline extension forwarding are checked by
injected creation failures before dispatching to the driver.

Reproduction and dependency overrides are in
[tests/gpu_pipeline_layouts/README.md](../../../tests/gpu_pipeline_layouts/README.md).

## Risks and next device check

No Android build, gameplay run, FPS/frametime/RAM measurement or CI monitoring was
performed. Publication uses a `[skip ci]` commit so the PR does not start the normal
Android workflow. No merge is requested or performed.

The cache adds CPU and metadata costs on misses, retains unique interfaces for the
device lifetime and serializes cold creation. No measured Android result yet shows
whether the observed reuse outweighs those costs on Qualcomm or Turnip. The current
scope has no draw-path lookup; it is consulted only while constructing a pipeline.

Before considering merge, compare the same scenes/settings/driver/cache state and
similar temperatures on the Moto G100. Separate cold creation from warmed gameplay;
record actual interface reuse and creation CPU time, descriptor pool/set counts,
frametime percentiles and memory. Check MK8/SMO graphics, MK11/DBFZ arena transitions,
Oceanhorn uploads and control games, with ordinary sets and push descriptors on
drivers where they are supported. A neutral or adverse device result is a valid
reason to revise or abandon the cache. Do not infer a fix for arena RAM growth from
this layout-only heap measurement.

## Code references

- [Device cache and structural keys](../../../app/src/main/cpp/skyline/gpu/pipeline_layout_cache.h): `Acquire`, `DescriptorProbe/Key`, `PipelineProbe/Key`.
- [GPU member ownership/order](../../../app/src/main/cpp/skyline/gpu.h): `pipelineLayouts` before `descriptor`.
- [Graphics creation](../../../app/src/main/cpp/skyline/gpu/graphics_pipeline_assembler.cpp): `AssemblePipelineAsync` and captured owner.
- [Graphics ownership](../../../app/src/main/cpp/skyline/gpu/graphics_pipeline_assembler.h): `CompiledPipeline`.
- [Compute creation](../../../app/src/main/cpp/skyline/gpu/interconnect/kepler_compute/pipeline_manager.cpp): `MakeCompiledPipeline`.
- [Descriptor allocator](../../../app/src/main/cpp/skyline/gpu/descriptor_allocator.cpp): `AllocateSet`, unchanged production implementation.
- [Vulkan descriptor/layout specification](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html): layout compatibility is distinct from descriptor-content validity.

No NNVK or Peanuts implementation was copied. Existing license notices are retained;
the new cache is MPL-2.0, matching these graphics sources.
