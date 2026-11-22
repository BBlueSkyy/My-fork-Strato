# Peanuts Native v0.1.6-1 Vulkan audit

## Result and scope

One small CPU optimization is retained: check `quickBindEnabled` before asking
whether two pipelines have compatible descriptor bindings. A disabled quick bind
always takes the full descriptor update, so calculating that compatibility was
unused work. No Peanuts code was copied. No FPS, device frametime, GPU-memory
reduction or game regression result is claimed.

Audit references, inspected on 2026-11-22:

- Strato `master`: `8bcaee0be950b44234cdd2767a11578c74db2602`.
- Peanuts tag `v0.1.6-1`: `fd018617eb963a47c733305ef67feb271cd16461`.
- Investigation branch: `perf/peanuts-vulkan-audit-20261122`.

The repositories have materially different inputs. Peanuts intercepts NVN calls
and uses an ahead-of-time shader library. Strato translates Maxwell command
state and shader binaries, including dependencies on constant buffers and live
texture/sampler metadata. A shader hash alone is therefore insufficient for a
Strato specialization or descriptor reuse key.

## Techniques found and Strato equivalents

Peanuts links below are pinned to the audited commit. Strato paths describe the
base above; the only runtime change is in `Maxwell3D::PrepareDraw`.

| Area | Concrete Peanuts implementation | Existing Strato equivalent and decision |
| --- | --- | --- |
| Pipeline keys | [`vka_pso.c`](https://github.com/igawa6/peanuts-native/blob/fd018617eb963a47c733305ef67feb271cd16461/dm/gfx/vk_android/vka_pso.c): word-at-a-time hash, last graphics-key hit before map lookup | `packed_pipeline_state.h::PackedPipelineStateHash` already uses XXH64 and omits unused trailing state. `Pipeline::LookupNext` tests the current pipeline and six transitions before the manager map. Do not replace it with another hash/cache without measured misses. |
| Layout lookup | `vka_layout_of`, `layout_slow` in that file: shader-combination shortcut and content-keyed layout map; `vka_draw.c` also retains the last shader combination | `GraphicsPipelineAssembler::AssemblePipelineAsync` creates one descriptor-set layout and pipeline layout per newly compiled pipeline. This is a concrete duplication opportunity across equivalent interfaces, not per-draw layout creation. No occurrence count or driver timing was captured. Defer sharing until complete value keys, ownership and benefit are tested. |
| Binding work | [`vka_bind.c`](https://github.com/igawa6/peanuts-native/blob/fd018617eb963a47c733305ef67feb271cd16461/dm/gfx/vk_android/vka_bind.c) resolves the shader library's binding records; [`vka_draw.c`](https://github.com/igawa6/peanuts-native/blob/fd018617eb963a47c733305ef67feb271cd16461/dm/gfx/vk_android/vka_draw.c) `push_key` deduplicates descriptor words; partial pushes are optional and disabled by default there | `Pipeline::SyncDescriptorsQuickBind`, `ConstantBufferDescriptorUsages`, `CheckBindingMatch` and `PrepareDraw` already support selective CBUF updates and descriptor reuse. `Textures` and `Samplers` already have indexed hot caches plus content-keyed stores. Keep their invalidation paths. |
| Vulkan state | `vka_draw.c` `vin_bind`, `DY_SET`, index/pipeline filters suppress redundant binds and batch adjacent vertex-buffer slots | Strato uses dirty-tracked pipeline substate and `StateUpdateBuilder`. `PipelineState::Flush/Refresh`, `PipelineStageState` and the existing transition path avoid unconditional rebuilds. Inspect state-update traces before adding another last-state filter. |
| CPU allocation | Peanuts keeps reusable descriptor-word arrays and scratch blocks | Strato already uses executor allocators, small/static vectors, cached buffer views and fence-owned resources. No general per-draw heap-allocation bottleneck was demonstrated. The retained change can avoid cold compatibility-map insertions when their result is unused. |
| Staging | [`vka_dev.c`](https://github.com/igawa6/peanuts-native/blob/fd018617eb963a47c733305ef67feb271cd16461/dm/gfx/vk_android/vka_dev.c) `vka_scratch`, `retire`, `sblk_trim`: linear mapped blocks, best-fit completed-block reuse, idle budget, separate upload/submission retirement | `MemoryManager` uses VMA. `AllocateStagingBuffer` still creates a VkBuffer/VMA allocation for each call. `MegaBufferAllocator` already provides linear allocation with fence recycling for other paths; it is not automatically a texture-staging pool. Measure VkBuffer/VMA call counts and outstanding bytes before adapting pooling. |
| Upload batching | `vka_dev.c` upload batches separate first-image initialization, retire staging on an upload timeline, flush by staging budget and before consumers | `Texture::SynchronizeHostInline` already records uploads in the executor command buffer; `SynchronizeHost` also has scheduler submission paths. Preserve the existing distinction. A separate transfer batch is not a drop-in replacement for the physical authority/dependency graph. |
| Deswizzle | [`vka_tex.c`](https://github.com/igawa6/peanuts-native/blob/fd018617eb963a47c733305ef67feb271cd16461/dm/gfx/vk_android/vka_tex.c) `upload_mt`: default three helpers above 256 KiB, rows split into approximately 64 KiB tasks, caller participates, join before ordered copy recording | Strato's `SynchronizeHostImpl` deswizzles serially over layers/mips, directly into mapped staging when no conversion is needed. Parallel independent output regions are a potential load-time/upload optimization. Need immutable source snapshots, correct mip/layer/GOB bounds, sparse-generation handling and an Android crossover measurement. |
| Copy reduction | Peanuts deswizzles into staging spans; partial dirty row/band uploads avoid gathering whole sparse textures in selected paths | Strato already writes directly to staging for equal guest/host formats and directly maps eligible linear UMA images; format conversion and sparse snapshots use intermediates. Removing the latter blindly risks corruption. Measure the actual conversion mix first. |
| Persistent cache/prewarm | `vka_pso.c`, [`vka_pwlist.h`](https://github.com/igawa6/peanuts-native/blob/fd018617eb963a47c733305ef67feb271cd16461/dm/gfx/vk_android/vka_pwlist.h): observed pipeline keys, optional profile seeds, startup workers, cache identity by driver/library | `PipelineCacheManager` already persists state bundles (v5); `PipelineManager` preloads them and drains assembly before gameplay. `GraphicsPipelineAssembler` already has an async pool and driver-specific Vulkan cache files. Do not implement a second observed-pipeline list. |
| Pipeline libraries | `vka_pso.c` splits graphics pipeline libraries, cache-only probes, fast links, background optimized versions and heavy-compile limiting | This requires reported extension/features and driver behavior tests. Peanuts itself has driver refusal/fallback logic. A large optional backend change, not suitable for this patch. No draw skipping or incomplete pipeline is adopted. |
| Cache memory | `vka_pckeep.h` / `vka_pso.c` reject stale shader-library cache content and throttle heavy preparations | Strato already scopes cache directories by title and Vulkan cache identity by vendor/device/driver/UUID. Pipelines are retained for existing raw references. Bounded eviction needs an ownership design and real memory measurements, not an arbitrary count limit. |
| Replay/recording | [`vka_worker.c`](https://github.com/igawa6/peanuts-native/blob/fd018617eb963a47c733305ef67feb271cd16461/dm/gfx/vk_android/vka_worker.c) FIFO replay; [`vka_enc.c`](https://github.com/igawa6/peanuts-native/blob/fd018617eb963a47c733305ef67feb271cd16461/dm/gfx/vk_android/vka_enc.c) copies command arguments into an encoder ring | Strato already has channel processing, `CommandRecordThread`, `ExecutionWaiterThread`, scheduler buffer slots, fence cycles and thread-local command pools. Do not transplant another worker. Contention requires wall-time versus thread-CPU traces. |
| Queue waits | [`vka_qsched.h`](https://github.com/igawa6/peanuts-native/blob/fd018617eb963a47c733305ef67feb271cd16461/dm/gfx/vk_android/vka_qsched.h) parks individual NVN queues, resumes around producers and learns CPU gates; also has timeout/pressure escape policies | Strato has a different command/dependency model. Learning and timeout policies do not establish Maxwell ordering correctness. No synchronization policy is transferred. |
| Profiling | [`vka_gprof.c`](https://github.com/igawa6/peanuts-native/blob/fd018617eb963a47c733305ef67feb271cd16461/dm/gfx/vk_android/vka_gprof.c) samples GPU timestamps at submission/barrier boundaries and attributes passes/transfers/dispatches; optional pass splitting adds substantial tiler work | Strato already has Perfetto CPU scopes for draws, recording, submission, uploads, readbacks and texture waits. `ExecutionWaiterThread`'s `GPU` scope is a host fence wait, not a GPU per-pass timestamp. Use existing CPU traces first; per-pass GPU timing remains separate future work. |

## Proven redundant work and retained change

Before the patch, `PrepareDraw` evaluated:

```cpp
activeDescriptorPipeline &&
(samePipeline || activeDescriptorPipeline->CheckBindingMatch(pipeline)) &&
constantBuffers.quickBindEnabled
```

`ConstantBuffers::Load`, unbinds, multiple distinct CBUF binds and executor flush
callbacks can disable quick bind. When disabled, compatibility cannot affect the
selected full update. It can still compare specialization vectors, hash/lookup a
pipeline pair, compare descriptor interfaces and allocate a new cache entry.
The patch evaluates `quickBindEnabled` first. The truth table is unchanged;
`SyncDescriptors`, `SyncDescriptorsQuickBind`, handle snapshots and descriptor
buffer inheritance are untouched. `CheckBindingMatch`'s only side effect is
memoization: it performs no resource synchronization or guest-memory reads.
Skipping an unused memoization may defer warming a pair until quick bind becomes
eligible, but cannot bypass a required compatibility check.

This is inspired by Peanuts' policy of avoiding repeated binding work. It is a
native Strato control-flow optimization, not a transplant of its descriptor ABI.

## Rejected experiment

Moving the existing compatibility cache lookup before specialization equality
was semantically plausible for immutable retained pipeline pairs. It accelerated
cached equal pairs, but the populated-cache/mismatching-specialization control
regressed from **1.350 ns to 4.592 ns** per isolated call. The additional map
lookup is unnecessary for those cheap rejections. That experiment was reverted
before the final validation and commit. `pipeline_manager.cpp` has no final diff.
See [rejected-cache-order.json](rejected-cache-order.json) for all raw rounds.
No new pointer cache or additional negative-specialization retention was added.

## Measurements and tests

The benchmark extracts the actual full compatibility function and actual
descriptor-update lambda from both the base commit and working tree. It uses
the pinned production `tsl::robin_map` and `PipelineSpecialization`. Context,
resource methods and descriptor-interface storage are facades; the full update
is a trivial stub. These are isolated CPU costs, not complete draw costs or GPU
measurements. There are 16 alternating target pairs and synthetic specialization
sizes; their real-game frequency has not been measured.

Final run: x86-64 host, GCC 14.2.0, C++20 `-O3 -DNDEBUG`, no LTO, pinned to
available CPU 0; 10,000 warmups, 11 rounds of 1,500,000 calls, alternating which
variant runs first. Raw rounds and host metadata are in
[binding-benchmark.json](binding-benchmark.json).

| Isolated case | Base median ns | Final median ns | Interpretation |
| --- | ---: | ---: | --- |
| Quick bind disabled, different pipelines, 48 queries | 67.399 | 1.165 | Avoids the unused compatibility check; does not measure the full descriptor update |
| Quick bind enabled, 48 queries | 65.711 | 65.737 | Existing eligible path, effectively unchanged |
| Different specialization, populated map | 1.152 | 1.159 | Rejection control preserved after reverting cache reorder |
| Cached equal, 12 queries | 16.411 | 16.687 | Unmodified compatibility function; harness variation |
| Cached equal, 48 queries | 63.121 | 65.296 | Unmodified compatibility function; no gain claimed |
| Cold equal, 48 queries, including map clear | 66.878 | 64.509 | Synthetic control including clear; no cold-path gain claimed |

Reproduction from repository root:

```sh
python3 tests/gpu_binding_match/benchmark.py --baseline 8bcaee0be950b44234cdd2767a11578c74db2602 --iterations 1500000 --rounds 11
python3 tests/gpu_tic_validation/run.py --probe-cache
python3 tests/gpu_tic_validation/run.py
```

Final host validation:

- Specialization suite: passed (normalized dependencies, deduplication,
  handles-first checks and selective metadata queries).
- Cache/descriptor probe: **118 checks, zero failures**, ASan/UBSan enabled.
- TIC executable suite: **1,186 checks, zero failures**, ASan/UBSan enabled.
- New coverage executes the production descriptor lambda: disabled quick bind
  with same/different pipelines always performs a full update; a disabled cold
  pair creates no compatibility entry; null previous pipeline falls back; enabled
  compatible pairs preserve partial/no-update behavior; incompatible stage,
  push-descriptor policy, descriptor interface and specialization force full
  updates, including repeated cache results.
- `git diff --check`: passed.

No Vulkan command, layout, descriptor contents, barrier, fence or lifetime code
changes. No local Vulkan Validation Layers or device gameplay run was performed
for this CPU predicate change. Existing Actions validation gates will run when
the draft PR opens; their outcomes are not implied by host results above.
The microbenchmark is optional, outside CI; semantic regressions use the existing
mandatory TIC/cache test gate.

## Android, Vulkan and correctness boundaries

The retained patch introduces no extension requirement and applies equally to
push-descriptor and ordinary-set devices, including the Snapdragon 870. It does
not establish the current feature set of any particular Qualcomm/Turnip driver.
Peanuts initialization itself requires timeline semaphores and dynamic rendering;
its device assumptions cannot be applied to older Strato-supported drivers.
Its vertex-input dynamic state and graphics pipeline libraries are feature-gated.

A future shared-layout key must include binding numbers, types, counts, stages,
immutable sampler values, push-descriptor flags and complete push-constant ranges;
shader identity alone does not prove descriptor-interface equivalence. Owners
must survive asynchronous creation and pending commands. See the authoritative
[Vulkan descriptor-set specification](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html).

Future GPU timings must verify queue-family `timestampValidBits`,
`timestampPeriod`, query availability and slot retirement. The CPU fence-wait
scope includes queueing and scheduling delay and cannot be labeled GPU render
cost. Pass splitting distorts Adreno tile load/store and binning costs. See the
[Vulkan queries specification](https://docs.vulkan.org/spec/latest/chapters/queries.html).

Texture aliases, HDR formats, physical regions, upload/readback order, Free Guest
Texture Memory, sparse generations and GC are untouched. The current base
already contains later texture/CBUF fixes, including the Oceanhorn and bindless
changes visible in its history. Older reports are not treated as reproductions
of this base. Draft #481 and its experimental physical tracing were neither
modified nor incorporated.

MK8, SMO, BOTW, RE5, DBFZ, MK11 and Oceanhorn 2 have **not** been revalidated in
this environment. Host tests are not a substitute for those results.

## Remaining opportunities, ordered by expected potential and risk

Expected potential is a hypothesis, not a measured ranking of device bottlenecks.

| Order | Opportunity | Expected impact | Complexity / risk | Required evidence before implementation |
| --- | --- | --- | --- | --- |
| 1 | Intern identical descriptor/pipeline layouts at new-pipeline construction | Lower creation/loading CPU, fewer duplicate Vulkan objects | Moderate / low-to-moderate with value keys and shared ownership | Count unique interfaces versus pipeline count; measure API time and retained bytes; test async destruction, push ranges, flags and immutable samplers with VVL |
| 2 | Reuse fence-completed texture staging with an idle-byte cap | Fewer buffer creations and transient allocation stalls in upload-heavy scenes | Moderate / moderate | Count allocations, live staging peaks and wait time; compare VMA behavior; test all fence/error/destruction paths and memory budget |
| 3 | Parallel large block-linear deswizzle into already-owned staging | Lower CPU upload bursts and loading time | Moderate / moderate | Byte-exact reference tests for mips/layers/3D/GOB spacing, sparse snapshots, ASan/UBSan and races; benchmark crossover, worker counts, thermal effects on SD870 |
| 4 | Avoid duplicate frontend translation/module creation or duplicate preload bundles | Less synchronous preparation and loading CPU | Moderate-to-high / moderate | New-variant and duplicate rates; complete specialization/runtime keys; immutable IR ownership or cloning; verify no stale CBUF/texture specialization |
| 5 | Attribute CPU preparation/compile/allocation versus real GPU passes | Better selection of subsequent performance work, no automatic FPS gain | Low for existing CPU traces, moderate for GPU queries | Perfetto captures with thread CPU and wall time; optional bounded query ring with no waits in the hot path; no artificial pass splitting for baseline |
| 6 | GPL fast linking and bounded optimized replacements | Potentially less pipeline stutter on supporting drivers | High / high | Extension/feature support on each target driver, cache/compile experiments, complete pipeline equivalence and lifetime tests; never skip draws |
| 7 | Change encoder/queue scheduling or add a transfer timeline | Unestablished contention/overlap benefit | High / high | First demonstrate a wait/recording bottleneck, then prove resource ordering and test scene changes, readbacks and remaps |

## License handling and device handoff

Peanuts' root `LICENSE` at the pinned revision is MIT; its vendored components
have separate licenses. Strato's root `LICENSE.md` is GPL-3.0 and these runtime
source files carry MPL-2.0 notices. This patch preserves existing notices and
imports no foreign implementation, helper, shader or vendored binary. Any later
code reuse needs a file-specific license/provenance review and retained notices.

Opening the draft PR triggers the existing `PR Build` workflow, whose APK task
runs `:app:assembleDevRelease` and verifies an ARM64-only signed APK. Refer to the
PR and workflow run for the final commit/run identifiers; no APK completion is
claimed here. The Actions build is not continuously monitored.

For the Moto G100 compare the base and candidate using the same driver,
settings, save/scene, cache state and similar phone temperature. Capture the
same route after warmup with FPS and frametime if available, especially scenes
with CBUF changes and pipeline switches. Check the seven named games for image
correctness and scene-change stability; longer runs can compare memory growth.
Keep Free Guest in the same setting for each A/B pair. A neutral device result
is plausible because this is a small CPU-only optimization and its hit rate is
unknown. Merge remains explicitly out of scope.
