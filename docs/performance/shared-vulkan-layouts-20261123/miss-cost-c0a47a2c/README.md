# PR #485 — remaining miss cost after c0a47a2c

Existing branch: `perf/shared-vulkan-layouts-20261123`.
Previous head: `c0a47a2cc9bfa9a15ca4f4c8b9b9f18ad3da48b8`.
Master reference: `e8e38237aa5993deeaaae9f01f39ea08500f18db`.
[Draft #485](https://github.com/BBlueSkyy/My-fork-Strato/pull/485).

## Outcome and scope

The reported **9.82 / 20.32 µs** all-unique control reproduces as **9.83 / 20.63 µs**
for master / c0a47a2c in this comparison. The revision takes **17.39 µs**, a 15.7%
reduction from c0, still 1.77 times master. The 50% mixed control decreases from
12.08 to 10.30 µs, still 4.6% above master. Repeated input and the historical
SpongeBob metadata keep their reuse benefits. This is not an integration approval
or an Adreno/FPS/frametime claim.

Only `pipeline_layout_cache.h` changes in production:

1. The primary index uses the existing library's power-of-two table with a final
   bit mix in `EntryHash`. This distributes binding/count high bits into the bucket
   bits. The descriptor structural hash and secondary pipeline index remain as in
   c0; no equivalence field, range, flag or extension state is dropped.
2. On a new descriptor interface, construct the caller's aliasing owner once and
   **move** the other owner into the index. Copying into the index then aliasing the
   temporary performed an additional increment/decrement of the control block.

The found-interface path retains c0's ownership sequence. A pilot also deferred the
alias construction and changed the hash feeding secondary variants; it produced
an adverse variant-control result. Those changes were discarded. The final table
below, including the variant control, measures the narrower revision rather than
selecting the best pilot numbers.

**No eviction, admission threshold or cache cap was introduced.** Safe designs and
the missing lifetime/identity prerequisite are evaluated below. No Android build,
new branch/PR, merge, instrumentation in the app, or CI monitoring was performed.

## Separately measured costs

The dedicated native `miss_profile.cpp` reads the exact reference header from git
and an exact current header, opening private visibility only for diagnostics. It
uses 128 unique valid interfaces. It precreates real Entry objects before table
insertion/lookup measurements. Stage timing has no VVL, sanitizers, Vulkan hooks,
new override or counters. Counters run separately after timing.

| Isolated stage, 128 operations | Median |
| --- | ---: |
| Descriptor structural hashing | 0.747 µs |
| Added final bit mix only | 0.192 µs |
| Owned descriptor-key construction | 0.452 µs |
| Full descriptor comparison, equal inputs | 0.459 µs |
| Full comparison, different inputs | 0.312 µs |
| 128 raw 256-byte allocation controls | 1.836 µs |
| c0 primary insertion, including table growth | 3.031 µs |
| Revised primary insertion, copy control | 1.581 µs |
| c0 lookup + insertion replay | 3.183 µs |
| Revised lookup + moved insertion replay | 1.502 µs |
| c0 lookup into a populated index, hit | 0.939 µs |
| c0 lookup into a populated index, miss | 0.360 µs |
| Uncontended mutex lock/unlock | 1.405 µs |
| Shared-owner increment/copy, release excluded | 0.686 µs |
| Shared native layout-pair construction | 11.722 µs |
| Shared Entry, key, ranges and native pair | 12.672 µs |
| Empty loop/clock control | 0.116 µs |

These are **separate controls, not an additive partition of Acquire**. The empty
loop/clock baseline is included; the table does not silently subtract it. Raw
allocation timing models the observed 240-byte Entry plus the host's 16-byte
control block, not construction of a complete shared Entry. Native pair timings
include real Vulkan calls. Destruction is outside individual stage timing.

Descriptor-key construction measures its complete owned binding fields and flags;
these small inputs fit inline. Primary pipeline flags and empty push ranges are
included in complete Entry construction. This 128-unique case creates no secondary
PipelineKey; its complete ranges/flags are covered by the variant workload and
native correctness tests. No expensive secondary-key construction is attributed
to a path that does not execute it. Input sorting is also not charged: these
fixtures already have ascending bindings. Unordered input retains the existing
normalization path.

The c0 index replay makes **605 calls to its table hash functor and four complete
comparisons**. The revised replay makes **256 hash-functor calls and zero complete
comparisons**. These are not 605/256 full traversals of the descriptor bindings:
c0's table functor reads the already computed structural hash. The pinned library
cannot reuse its stored 32-bit tag for prime-policy rehash on this 64-bit host; it
can for the tested power-of-two sizes. This avoids repeated Entry reads during
growth and prime bucket arithmetic. The new functor applies the bit mix for lookup
and insertion; complete field comparison still handles collisions.

The raw-pointer index control measured 2.465 µs, versus 3.031 µs for c0's shared
index insertion. It was not adopted: maintaining exportable shared ownership would
need another owner/index structure or another lookup. The measured partial saving
does not establish an end-to-end benefit sufficient to justify that change.

The mutex control is about 1.405 µs. The full no-lock c0 variant is retained in the
raw paired data, solely on one thread. Neither test measures contention or permits
removing production synchronization. Hashing/key construction/comparison alone do
not explain the remaining gap. Native creation, allocation/shared lifetime,
insertion/growth, input preparation and locking all contribute. No allocator arena,
spin lock, hash-only equality or device-specific shortcut was added without proof.

## Comparative benchmarks

Host: x86-64, Clang 19.1.7, C++20 `-O3 -g -DNDEBUG`, llvmpipe LLVM 19.1.7. Twelve
rounds rotate master, current, c0 and c0-no-lock order; 300 fresh-cache epochs per
variant/round after 25 warmups. Host Boost is 1.83; allocator/control-block
figures describe this host, not an Android libc++ ABI. Cases run sequentially on CPU 0, with no competing
compiler/benchmark. Timing executables have no VVL, ASan/UBSan, new override,
allocation counters or Vulkan interception. Master uses its inline two-object
construction sequence, without charging it cache/shared-owner allocations.

| Creation per sequence | Master | c0a47a2c | Revised |
| --- | ---: | ---: | ---: |
| Historical SpongeBob definitions | 14.39 µs | 11.53 µs | 10.29 µs |
| 128 unique interfaces | 9.83 µs | 20.63 µs | 17.39 µs |
| 128 requests, eight interfaces | 9.70 µs | 3.83 µs | 3.69 µs |
| 128 requests, 50% repeated | 9.85 µs | 12.08 µs | 10.30 µs |
| Eight descriptor / 128 push interfaces | 9.93 µs | 13.87 µs | 13.63 µs |
| 2,048 unique interfaces | 158.92 µs | 322.48 µs | 294.54 µs |

The historical fixture has 66 requests / 30 unique definitions, and **still creates
30 descriptor and 30 pipeline layouts with 36 hits**. It is a historical SpongeBob
capture, not current-master gameplay. Repeated input uses eight interfaces, mixed
input uses 64 followed by 64 shuffled repeats. The variant control has eight
descriptor interfaces and 128 unique pipeline ranges: descriptor reuse alone.
Growth uses 2,048 dense unique interfaces. The last four synthetic controls do not
estimate real game frequency. No whole-pipeline compilation or submission speedup
is inferred from layout-only timing.

| Entire lifecycle, including destruction | Master | c0a47a2c | Revised |
| --- | ---: | ---: | ---: |
| Historical SpongeBob definitions | 17.14 µs | 13.61 µs | 12.40 µs |
| 128 unique interfaces | 15.90 µs | 29.79 µs | 26.44 µs |
| 128 requests, eight interfaces | 15.74 µs | 4.92 µs | 4.70 µs |
| 128 requests, 50% repeated | 15.97 µs | 16.90 µs | 15.15 µs |
| Eight descriptor / 128 push interfaces | 15.94 µs | 22.45 µs | 22.16 µs |
| 2,048 unique interfaces | 257.58 µs | 480.92 µs | 455.18 µs |

Lifecycle timing includes the equally reserved result arrays and cache/result
teardown. Object/heap diagnostics use another executable and logical device; they
are not mixed into creation timing. The published no-tcache snapshots run five
samples in a fresh process per case with
`GLIBC_TUNABLES=glibc.malloc.tcache_count=0`. Snapshot deltas use glibc
`mallinfo2().uordblks` after reserving the result arrays. They include cache metadata
and driver allocations visible to glibc, not GPU allocation size, RSS or Android
RAM. Normal-tcache samples also remain in the raw rounds for transparency.

| Live heap, no-tcache diagnostic | Master | c0a47a2c | Revised |
| --- | ---: | ---: | ---: |
| Historical SpongeBob definitions | 82,528 B | 52,752 B | 52,688 B |
| 128 unique interfaces | 104,448 B | 145,456 B | 145,440 B |
| 128 requests, eight interfaces | 104,448 B | 9,120 B | 9,104 B |
| 128 requests, 50% repeated | 104,448 B | 72,784 B | 72,736 B |
| Eight descriptor / 128 push interfaces | 104,448 B | 100,384 B | 100,368 B |
| 2,048 unique interfaces | 1,671,168 B | 2,375,872 B | 2,326,544 B |

The 16-byte reduction in the unique control is **not a meaningful memory gain**.
Both caches still exceed master there. Growth retains about 49 KB less than c0;
there is no universal memory improvement or enforced limit.

| Ordinary C++ new calls, metadata diagnostic | c0a47a2c | Revised |
| --- | ---: | ---: |
| Historical SpongeBob definitions | 58 | 58 |
| 128 unique interfaces | 139 | 136 |
| 128 requests, eight interfaces | 10 | 12 |
| 128 requests, 50% repeated | 73 | 71 |
| Eight descriptor / 128 push interfaces | 254 | 256 |
| 2,048 unique interfaces | 2067 | 2060 |

The 128 Entry allocations remain. Table growth drops from eleven to eight
allocations at 128 unique interfaces. Smaller tables can have **more** growth
allocations: the eight-interface control rises from ten to twelve total calls.
There are no new allocations on ordinary ordered hits. These counters exclude
C malloc/calloc and therefore do not mean zero driver allocation for master.

## Safe growth-limit evaluation

**Proven current behavior:** `DescriptorAllocator::DescriptorPool::layoutSlots`
keys inactive and active slots by native descriptor-layout handle. Slots/active
wrappers retain the pool, not the layout owner. `ObserveDescriptorRetention`
executes this exact allocator with real Vulkan and observes:

```text
inactive_sets_allocated=1 layout_owner_count=1 expired_after_cache_teardown=1
```

One inactive set remains allocated in the still-live pool, while the only layout
owner is the cache. Dropping the cache then expires the weak owner. The observation
uses no stale/recycled handle, issues no invalid GPU commands and produces no VVL
error. It demonstrates missing **application ownership/identity protection**, not
a reproduced Qualcomm/Turnip recycled-handle failure. It must not be described as
a Vulkan requirement that layouts always outlive allocated sets.

A `use_count == 1` rule could remove that canonical layout while a bucket still
uses its native value as a key. If a later incompatible layout receives the same
value, the current allocator can find the old slot bucket. Shader/pipeline owners
alone do not account for that pool lifetime. Weak caching or a general LRU would
therefore require an additional prerequisite.

| Policy | Safety/benefit with current code | Decision |
| --- | --- | --- |
| Evict primary Entries by count/LRU/use_count | Untracked pool buckets can outlive the owner; recycled-handle identity is unprotected | Do not implement |
| Stop admission and release uncached pairs normally | The same identity problem applies outside admitted entries | Do not implement |
| Stop indexing but pin all native owners elsewhere | Bounds only index size; retained objects still grow and later repeated interfaces create duplicates | No demonstrated benefit |
| Limit only secondary pipeline interfaces, keep all descriptor roots | Exported shared owners remain valid and descriptor bucket identity stays pinned; primary growth is unchanged | Architecturally safe narrow option, not enabled without evidence |
| Strong descriptor owner per pool bucket, then admission/LRU | Buckets pin canonical identity through active/inactive sets and pool resets; needs owner propagation through allocation paths | Safe future design to validate |
| Generation-safe bucket identity, then admission/LRU | Separates recycled handles, but every cached/bypass/helper caller must carry the correct generation | Alternative future design, not implemented |

The secondary-only option cannot solve the all-unique primary control. Moreover,
Maxwell's render-area push range, compute's empty ranges and helper shaders' fixed
ranges do not establish real secondary-range explosion. An arbitrary cap for that
synthetic control would not be a justified production optimization.

The graphics and compute pipeline managers retain their compiled variants. While
those owners are live, erasing an index entry cannot free its coallocated key/layout
Entry; a hard total-memory bound cannot discard resources still in use. Bounding
lookup metadata is distinct from bounding all live pipeline/native memory.

A safe future experiment would first pass an existing strong descriptor owner (or
validated generation) through `DescriptorUpdateInfo`/helper allocation paths to the
bucket, and validate pool reset, replacement, inactive slots and pending fences.
Only then measure bounded admission/LRU with realistic Android reuse and retention,
preserving existing admitted hits and bypassing admission when necessary. No extra
owner lookup should be inserted per draw merely to discover that owner. A budget
must account for variable key storage, indexes and pinned resources and cannot
force-release live resources. There is no justified threshold from current device
data, so the production policy remains strong device-lifetime retention.

## Correctness and validation

Only the primary-table hash projection, growth policy and new-interface ownership
transfer change. Every descriptor binding/type/count/stage/flag and pipeline
flag/range remains in full equality. The descriptor hash feeding secondary
pipelines is unchanged. Sorting, pNext/immutable-sampler bypass, graphics/compute,
async ownership, failure/retry and device isolation retain their previous design.
No Vulkan extension requirement, cache format, quick-bind decision, descriptor
contents, texture/alias/mipmap/texel-buffer/GC/synchronization/game-fix code changes.
The c0 helper-call corrections remain intact.

**2,426 checks passed with ASan/UBSan and real Vulkan; zero Khronos VVL 1.4.309
synchronization-validation errors.** The added collision fixture copies the exact
production cache and forces both primary EntryHash projections to zero, leaving
stored structural hashes and full equality intact. It keeps 64 distinct interfaces
separate and reuses reordered/equal definitions. This adds many pairwise assertions,
not thousands of different game scenarios.

Existing focused native cases also pass: 24 simultaneous first users, eight-worker
1,024-interface growth, stable primary/secondary owners, deferred actual compute
compilation after cache teardown, actual graphics/compute creation and output bits,
ordinary/push descriptors, active/completed slots, flags/extensions/samplers and
Vulkan failure/retry. Test service/push-data facades are unchanged; this is not the
complete Android renderer or guest Maxwell execution.

Only this directly affected native suite was rerun; the unchanged TIC, quick-bind
facade probe, preload and extraction tests were not repeated from c0. LeakSanitizer
is disabled under ptrace. No ThreadSanitizer, injected C++ allocator failures, full
Android link/build or Qualcomm/Turnip gameplay validation is claimed. The mutex
remains; stress tests and ASan/UBSan do not replace a data-race detector.

[Raw rounds/profile stages](rounds.jsonl), [heap diagnostics](heap-no-tcache.jsonl),
[validation log](native-validation.txt), [exact summaries/protocol/checksums](validation.json).

## Reproduction and source references

```sh
CXX=clang++ python3 tests/gpu_pipeline_layouts/run.py --benchmark --review-ref c0a47a2c --suite --miss-profile --build-dir /tmp/layout-misses > /tmp/layout-miss-rounds.jsonl
CXX=clang++ python3 tests/gpu_pipeline_layouts/run.py --build-dir /tmp/layout-native
GLIBC_TUNABLES=glibc.malloc.tcache_count=0 /tmp/layout-misses/diagnostic /tmp/layout-misses/workloads/unique.txt
```

Use the same diagnostic command for every fixture, in a fresh process per case;
its five samples are already emitted internally. Dependency/environment overrides
are in [the test README](../../../../tests/gpu_pipeline_layouts/README.md). The
profiler runs for the unique case only; its code rejects duplicate descriptor
interfaces. Build profile/timing/diagnostic binaries before measuring, without
competing CPU jobs.

- [Primary cache](../../../../app/src/main/cpp/skyline/gpu/pipeline_layout_cache.h): `FinalizeHash`, `EntryHash`, `Acquire`; full keys and ownership structs.
- [Dedicated profiling](../../../../tests/gpu_pipeline_layouts/miss_profile.cpp), [paired benchmark](../../../../tests/gpu_pipeline_layouts/benchmark.cpp), [shared recipe loader](../../../../tests/gpu_pipeline_layouts/benchmark_inputs.h).
- [Allocator](../../../../app/src/main/cpp/skyline/gpu/descriptor_allocator.h): `DescriptorPool::layoutSlots`, `DescriptorSetSlot`, `ActiveDescriptorSet`; [implementation](../../../../app/src/main/cpp/skyline/gpu/descriptor_allocator.cpp): `AllocateSet`, reset/replacement.
- [Native execution/retention](../../../../tests/gpu_pipeline_layouts/execution.h): `ObserveDescriptorRetention`, actual graphics/compute and deferred compilation; [collision/concurrency cases](../../../../tests/gpu_pipeline_layouts/tests.cpp).
- [Maxwell manager](../../../../app/src/main/cpp/skyline/gpu/interconnect/maxwell_3d/pipeline_manager.cpp): `MakeCompiledPipeline` render-area ranges, map ownership/preload; [compute manager](../../../../app/src/main/cpp/skyline/gpu/interconnect/kepler_compute/pipeline_manager.h): `FindOrCreate` retained map.
- [Helpers](../../../../app/src/main/cpp/skyline/gpu/shaders/helper_shaders.cpp): fixed push ranges and allocation consumers, unchanged.
- [Pinned robin_hash source](https://github.com/Tessil/robin-map/blob/f8e0f679d2fad8b47f6300f25ab844fd49a1f7e5/include/tsl/robin_hash.h): `USE_STORED_HASH_ON_REHASH`, `rehash_impl`, `insert_impl`; existing dependency and license unchanged.
- [vkDestroyDescriptorSetLayout](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyDescriptorSetLayout.html), [vkDestroyPipelineLayout](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyPipelineLayout.html), [VkDescriptorSetAllocateInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkDescriptorSetAllocateInfo.html): distinguish Vulkan object rules from this allocator's handle identity.

The remaining cold/mixed penalty, unbounded retention and lack of Android evidence
remain explicit reasons to keep this Draft. Publication uses `[skip ci]`; no build
or merge is requested. No NNVK/Peanuts code or new dependency is imported.
