> Continued investigation: [remaining miss cost and growth-policy review](miss-cost-c0a47a2c/README.md).
> The report below records **c0a47a2c**; its source/checksum snapshots are preserved.
> [Source at that commit](https://github.com/BBlueSkyy/My-fork-Strato/tree/c0a47a2cc9bfa9a15ca4f4c8b9b9f18ad3da48b8).

# Reuse structurally identical Vulkan layouts — PR #485 review

Base: `e8e38237aa5993deeaaae9f01f39ea08500f18db` (`master`, unchanged).
Existing branch: `perf/shared-vulkan-layouts-20261123`.
Previous PR revision: `e8e6b089e7e2dc39b5980671afd3c086c355b248`.
[Draft #485](https://github.com/BBlueSkyy/My-fork-Strato/pull/485), building on the
layout opportunity from [#482](https://github.com/BBlueSkyy/My-fork-Strato/pull/482).

## Review outcome

The original 9.74 → 28.73 µs all-unique regression reproduces in this paired run as
**9.82 µs master / 29.19 µs previous PR**, for 128 requests. The revision takes
**20.32 µs**, about 30% below the previous cache but still **2.07 times master**.
Metadata allocation calls decrease from **650 to 139**. Reuse is preserved: the
historical 66-definition fixture still creates only 30 of each layout type.

The cache benefits duplicate-heavy input, but not every workload. The 50% mixed
control remains slower than master, and 2,048 unique interfaces retain additional
heap and take twice the creation time. **This review does not establish that the
cache is ready to integrate or improves Adreno frametime/FPS.** Keep the PR Draft
until device measurements justify the tradeoff.

The review also found and fixed a concrete compile regression: five blit/clear
helper call sites dereferenced the newly borrowed Vulkan handles as if they were
RAII wrappers. A native Vulkan-Hpp compile reproduced the error; the harness now
extracts and compiles those actual consumer statements. No Android build was run.

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

## Cost investigation

All numbers below concern layout creation/lookup, not whole pipeline compilation.
The diagnostic source is generated directly from the previous commit; the reference
algorithm is not reimplemented. A test-only visibility change exposes its hash/key
helpers. A separate reference copy removes only the mutex, used on one thread.
The production mutex remains intact.

| 128 unique interfaces | Observation |
| --- | ---: |
| Previous complete Acquire path | 29.19 µs |
| Same previous code, mutex removed, single thread only | 28.47 µs |
| Isolated descriptor hashing, all 128 inputs | 0.524 µs |
| Isolated pipeline-range hashing, 128 inputs | 0.127 µs |
| Isolated owned descriptor-key construction | 1.526 µs |
| Isolated uncontended lock/unlock loop | 1.341 µs |
| Previous ordinary C++ new calls / cumulative requested bytes | 650 / 46,672 B |
| Revised ordinary C++ new calls / cumulative requested bytes | 139 / 55,928 B |

These isolated loops have different instruction/cache/allocator contexts and **do
not add up to an exact decomposition of Acquire**. The paired no-lock ablation
rules out the mutex as the principal cause in this uncontended workload. It says
nothing about real preload contention. Hashing small interfaces is inexpensive
relative to the full regression, so no hash shortcut or collision-only comparison
was introduced.

The previous common miss allocated a binding vector, two shared ownership objects,
two map nodes and occasional bucket arrays: five allocations per interface plus
ten growth allocations, or 650 total. It also searched and populated two indices
for the common one-descriptor/one-pipeline interface. In the revision, the 128 small
interfaces use one Entry allocation each plus eleven flat-table growth allocations.
There is no temporary owned key, second node or separate descriptor control block
on that path. Larger bindings and secondary interfaces can still allocate.

Fewer allocations do not imply fewer cumulative requested bytes: inline keys and
flat-table growth increase that counter. The no-tcache live heap snapshot for this
control falls from 155,760 to 145,456 B, about 6.6%, and remains above master's
104,448 B. The push-variant control has a small **96-byte increase** over the old
cache. Neither memory benefit is universal.

A distribution check of the unchanged descriptor hash found 128 distinct hashes,
but only two occupied residues modulo 256 (64 inputs each), versus 96 modulo 257
(maximum three). The new index uses the existing library's **prime growth policy**,
not its default power-of-two policy. Full value equality remains mandatory.

## Revised design and correctness audit

[`PipelineLayoutCache`](../../../app/src/main/cpp/skyline/gpu/pipeline_layout_cache.h)
remains device-local and immutable after publication:

- `DescriptorProbe` borrows input binding records. `DescriptorKey` owns all binding
  numbers, types, counts, stage masks and descriptor flags. Ascending inputs avoid
  sorting; other inputs are normalized using an owned temporary array. Up to four
  bindings fit inline; larger interfaces use dynamic storage. No pointers to caller
  arrays enter a stored key.
- The primary `Entry` combines this key, complete pipeline flags/ranges and a
  `Layouts` object containing both Vulkan objects. Its aliasing shared pointer owns
  the Entry/control block, not merely the address of the embedded Layouts. Entries
  cannot move; table rehash moves only shared pointers. One push range fits inline.
- A second index is used **only** for additional pipeline interfaces of an already
  canonical descriptor. Its key is the stable Entry address, every pipeline flag,
  and complete stage/offset/size ranges. It remains hashed rather than scanning an
  ever-growing list. Push-range ordering is compared exactly; equivalent reordered
  ranges may miss reuse, but cannot produce a false hit.
- A secondary `Layouts` owns its primary Layouts; the primary has no back-reference.
  The secondary's descriptor pointer borrows that owned primary's inline object.
  This is acyclic. Pipeline layouts die before their descriptor owners. The tested
  secondary survives cache teardown and actually compiles a compute pipeline after
  teardown in a deferred task. A weak-root check then proves release, not retention
  through an ownership cycle.
- One mutex protects both indices, first creation and publication. Hashing and
  normalization use caller-owned immutable inputs outside the mutex. The mutex is
  retained across Vulkan creation; this prevents duplicate concurrent first use but
  serializes unrelated cold creates. No performance claim about contention is made.
- A failed first pipeline creation destroys the unpublished descriptor/pipeline
  group instead of retaining a partial descriptor entry. Retry cannot hit failure
  state. Failed secondary creation leaves the valid primary intact. A failed map
  insertion releases local RAII owners. The flat table stores noexcept-movable and
  swappable shared pointers, satisfying its documented strong-exception condition.
  Vulkan creation errors are injected in tests; C++ allocation failures are reviewed,
  not injected.
- Every descriptor/pipeline `pNext` and every immutable-sampler pointer bypasses
  interning and forwards the original data. No extension/sampler field is silently
  ignored. Sampler ownership is still the caller's existing responsibility. Unknown
  flag bits participate in admitted keys and are forwarded unchanged; tests reject
  unsupported cases in a dispatch shim before the driver.
- Graphics and compute share an index; bind point is not part of Vulkan layout
  identity. All shader stage masks still are. The native test passes one descriptor
  set between actual production graphics/compute creation paths, with different
  push-constant interfaces and distinct pipeline layouts, and checks output bits.
- Canonical handles remain strong until GPU/cache teardown. There is **no runtime
  eviction, size cap, persistence or cross-device sharing**. The cache outlives the
  handle-keyed `DescriptorAllocator` pools by GPU member order. Weak retention or
  eviction without redesigning those buckets could let a recycled handle identify
  an incompatible slot. Unique interface/range counts therefore grow memory until
  teardown; the 2,048-interface control measures that limitation explicitly.
- Invalidation is structural: changed flags/bindings/ranges choose another entry;
  resource contents, shader specialization and descriptor validity are handled by
  their existing mechanisms. A rebuilt GPU/device needs its own cache. Teardown
  requires stopped Acquire callers and a live VkDevice for remaining shared owners,
  as with the previous RAII objects. It is not safe to replace the VkDevice under an
  existing cache. No cache-clear or device-recovery protocol was added.

No Vulkan feature or extension dependency was added. The existing Boost.Container
and tsl::robin_map target dependencies already cover the headers used here.
`CanUsePushDescriptors`, `noPushDescriptors`, quick bind, descriptor contents,
Float/UINT/SINT specialization, pipeline/shader keys and cache formats are unchanged.
The five helper edits only adapt consumers to the already changed handle type.
Query-resolve and conditional-compare helper objects still own individual RAII
layouts; their correctly dereferenced fields are deliberately unchanged.

Textures, compatible aliases, mipmaps, texel-buffer contents, Free Guest, GC,
barriers, queue submission and game fixes receive no production changes. The known
preexisting backing limitation is not fixed or declared green by this work.

## Comparative results

x86-64, Clang 19.1.7, C++20 `-O3 -DNDEBUG`, llvmpipe LLVM 19.1.7, CPU affinity 0.
Twelve rounds rotate the four variants; 25 warmup epochs and 300 fresh-cache epochs
per variant/round. Each epoch retains all results until its end. Cases run
sequentially, after compilation and functional tests, without competing benchmark
jobs. Timing has no VVL, ASan, hooks, operator-new override or allocation counters.
Master uses its inline two-object RAII construction sequence, without adding shared
ownership or cache keys to the baseline.

Creation includes lookup, creation and returned ownership; lifecycle additionally
includes cache destruction and reserved owner-vector construction/destruction.
Identical owner vectors are reserved for all variants. These are isolated native
workloads, not PSO compilation, a Vulkan submission benchmark or gameplay.

| Sequence | Requests / unique DSL / unique PL | Master | Previous PR | Revised |
| --- | ---: | ---: | ---: | ---: |
| Historical capture definitions | 66 / 30 / 30 | 13.64 µs | 12.96 µs | 11.04 µs |
| Unique small interfaces | 128 / 128 / 128 | 9.82 µs | 29.19 µs | 20.32 µs |
| Repeated interfaces | 128 / 8 / 8 | 9.61 µs | 4.99 µs | 3.84 µs |
| Mixed, 50% repeat | 128 / 64 / 64 | 9.76 µs | 16.56 µs | 12.16 µs |
| Unique push variants | 128 / 8 / 128 | 10.17 µs | 18.86 µs | 14.64 µs |
| Unique growth control | 2048 / 2048 / 2048 | 161.02 µs | 501.40 µs | 329.58 µs |

The mixed input consists of 64 different interfaces followed by one shuffled repeat
of each. The repeated input uses eight interfaces over 128 requests (120 hits).
The variant control has eight descriptor interfaces but 128 different pipeline
push ranges: it saves descriptor objects, not pipeline layouts. Growth uses 2,048
unique, dense three-binding interfaces with valid descriptor counts/stage masks.
These synthetic cases do not estimate any game's reuse rate.

| Lifecycle per sequence, including destruction | Master | Previous PR | Revised |
| --- | ---: | ---: | ---: |
| Historical capture definitions | 16.32 µs | 16.77 µs | 13.18 µs |
| Unique small interfaces | 15.94 µs | 46.60 µs | 30.14 µs |
| Repeated interfaces | 15.47 µs | 6.45 µs | 4.84 µs |
| Mixed, 50% repeat | 15.85 µs | 25.63 µs | 17.14 µs |
| Unique push variants | 16.65 µs | 30.13 µs | 23.47 µs |
| Unique growth control | 267.86 µs | 814.60 µs | 499.68 µs |

Object counts, ordinary C++ allocation calls and heap are measured in a separate
instrumented executable/device. It overrides ordinary new only for diagnostics;
master's zero calls mean zero C++ metadata new calls, **not zero driver allocation**.
The initial PR and normal-tcache samples can obscure freed chunks in glibc tcache.
The table below instead uses five diagnostic samples per case in a separate process
with `GLIBC_TUNABLES=glibc.malloc.tcache_count=0` set before process startup.
This setting is **not** applied to the timed executable. `mallinfo2().uordblks`
deltas include driver allocations visible to glibc and cache metadata, relative to
reserved owner arrays. They are not GPU allocations, RSS or Android RAM. Driver
allocation callbacks reported zero and are not used to estimate memory.

| No-tcache live heap delta | Master | Previous PR | Revised |
| --- | ---: | ---: | ---: |
| Historical capture definitions | 82,512 B | 53,632 B | 52,768 B |
| Unique small interfaces | 104,448 B | 155,760 B | 145,456 B |
| Repeated interfaces | 104,448 B | 9,744 B | 9,120 B |
| Mixed, 50% repeat | 104,448 B | 77,888 B | 72,784 B |
| Unique push variants | 104,448 B | 100,304 B | 100,400 B |
| Unique growth control | 1,671,168 B | 2,462,624 B | 2,375,872 B |

[Paired rounds and initial normal-tcache diagnostics](review-rounds.jsonl),
[no-tcache heap diagnostics](heap-no-tcache.jsonl),
[exact summaries/protocol/source SHA-256](validation.json).
The original PR's 11-round results remain preserved as
[capture](capture-rounds.json), [unique](unique_control-rounds.json) and
[initial metadata](initial-validation.json), all tied to `e8e6b089` rather than the
revised source. Their allocator configuration differs from the new no-tcache table;
do not compare those heap numbers as a code-only change.

## Validation and limits

- **281 checks** in the native production cache/integration suite with ASan/UBSan,
  real Vulkan and Khronos VVL 1.4.309 synchronization validation: **zero errors**.
- Actual graphics description/assembly/async code, compute creation/ownership and
  DescriptorAllocator implementation compile and execute. Exact compute integer
  bits, graphics pixels, push constants and active/completed descriptor slots pass.
- Actual blit/clear allocation/bind/push-constant statements now compile against
  the production CompiledPipeline type. Their push-data/platform types are facades;
  the complete helper shader translation unit and Android application are not built.
- Twenty-four simultaneous first users create one pair. Eight workers grow 1,024
  descriptor interfaces and 2,048 pipeline interfaces while verifying canonical
  primary/secondary owners, then release them. Rehash, failure/retry, structural
  differences, device isolation and extension/sampler bypass checks pass.
- Primary and secondary ownership after cache destruction, deferred real compute
  pipeline compilation, and weak-root expiry pass with balanced create/destroy counts.
- Existing specialization/cache/descriptor probe: **129 checks**, Float/UINT/SINT
  variants and 20,000 facade equivalent-handle draws with zero compiles/flushes.
- Existing production preload lifetime success, corruption and exception cases pass.
- Full TIC 1,186 checks and extraction were already validated in the initial PR;
  those unchanged paths were not rerun. The revision reran only the directly relevant
  native cache/integration, cache probe and preload checks. New checks prompted
  additional targeted runs; failed harness-compilation attempts are not counted green.
- LeakSanitizer remains disabled under ptrace. No LSan, ThreadSanitizer, full guest
  execution, full Android link/build, Qualcomm/Turnip or thermal/FPS/frametime test
  is claimed. ASan/UBSan do not constitute a data-race detector.

Logs: [native](native-validation.txt), [cache probe](cache-probe.txt),
[preload](preload.txt), [expected helper compile failures](helper-negative-control.txt). [Reproduction instructions](../../../tests/gpu_pipeline_layouts/README.md).

The cache is consulted on pipeline creation, not per draw. Its cold miss regression,
serialized creates and device-lifetime growth remain material risks. The measured
benefits preserve the reuse opportunity but do not establish a universal CPU or
memory improvement. Before integration, measure reuse, layout creation CPU time,
descriptor sets/pools, heap and frametime on the Moto G100 with the same scenes,
drivers, caches and thermal conditions. Include MK8/SMO, MK11/DBFZ arena changes and
control games. Do not infer a fix for MK11 arena RAM growth from these heap deltas.
No Android build, new PR, merge or CI monitoring was performed; publication uses
`[skip ci]` on the existing branch.

## Source and specification references

- [PipelineLayoutCache](../../../app/src/main/cpp/skyline/gpu/pipeline_layout_cache.h): `DescriptorKey`, `Entry`, `Layouts`, `Acquire`, secondary index.
- [GPU member order](../../../app/src/main/cpp/skyline/gpu.h): `pipelineLayouts` before `descriptor` and after VkDevice.
- [Graphics assembler](../../../app/src/main/cpp/skyline/gpu/graphics_pipeline_assembler.cpp): `AssemblePipelineAsync`; [ownership](../../../app/src/main/cpp/skyline/gpu/graphics_pipeline_assembler.h): `CompiledPipeline`.
- [Compute manager](../../../app/src/main/cpp/skyline/gpu/interconnect/kepler_compute/pipeline_manager.cpp): `MakeCompiledPipeline`.
- [Helper consumers](../../../app/src/main/cpp/skyline/gpu/shaders/helper_shaders.cpp): `BlitHelperShader::Blit`, `ClearHelperShader::Clear`.
- [Descriptor allocator](../../../app/src/main/cpp/skyline/gpu/descriptor_allocator.cpp): `AllocateSet`, unchanged.
- [Pinned flat-set source](https://github.com/Tessil/robin-map/blob/f8e0f679d2fad8b47f6300f25ab844fd49a1f7e5/include/tsl/robin_set.h): exception guarantees and `robin_pg_set`; existing MIT dependency, no copied implementation.
- [VkPipelineLayoutCreateInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineLayoutCreateInfo.html): sets, flags, push ranges and descriptor-layout access lifetime.
- [VkDescriptorSetLayoutBinding](https://docs.vulkan.org/refpages/latest/refpages/source/VkDescriptorSetLayoutBinding.html): stage masks, graphics/compute sharing and immutable-sampler lifetime.
- [vkDestroyPipelineLayout](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyPipelineLayout.html): same-device lifetime and host synchronization.
- [Descriptor-set compatibility](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html): layout compatibility does not prove descriptor-content validity.

No NNVK or Peanuts implementation was copied. Existing license notices are preserved;
the new cache is MPL-2.0, matching the graphics code.
