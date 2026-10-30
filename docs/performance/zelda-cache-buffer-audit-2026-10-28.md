# Strato: historical regression and current CPU lookup optimizations

Date: 28 October 2026. Upstream base: master
`9935f13349c00d732ae599f4f90866b5b79629f5`.
Reference device: Motorola Moto G100, Snapdragon 870, Adreno 650, Android 12.

## Conclusions and evidence boundaries

Two bounded CPU optimizations are implemented: restore render-pass cache hits
for absent resolve attachments, and stop buffer overlap fallback scans once
earlier disjoint mappings cannot overlap. Production changes comprise eight
added lines and one removed line. Neither changes Vulkan create infos, resource
contents, aliases, barriers, layouts, descriptor semantics or fence ordering.

Both modified source files have **identical Git blobs in the known-good
28bcb957 and current master**. These optimizations therefore do not identify
the origin of the later PR #294 regression. They remove independently verified
work in current code. No Zelda, Marvel or UE4 FPS/compatibility improvement is
claimed without device tests.

## Historical build reconstruction

| Actions run | Workflow head | Actual Strato checkout | Shader compiler |
|---|---|---|---|
| 39351576465 | fab103aa594cf6b4ec3d500aa05aaeeeb8419486 | 28bcb957a9f018212f3b0a2e5628028bdb22bf69 | 6c804797a6e073c925958cedd6d5eb8114a3c65d |
| 39360176180 | a5a0c4d68b1dc6dc502f759f528cbdc28d9f39bd | e7290a9b5f4c346a2e5198210d022a8da151f6ab | dcb938866f4234963fcbc199f2443438d8ef6f07 |
| 39410783710 | 9a6b48bead242bdcc76a47dd4fbbaeeff33d114e | 9a6b48bead242bdcc76a47dd4fbbaeeff33d114e | dcb938866f4234963fcbc199f2443438d8ef6f07 |

The user reported good performance in PR #293 and the later baseline
28bcb957, and slow performance with e7290a9b. That bounds a package, not an
individual cause. The 28bcb957 to e729 interval contains:

1. bcae2588: PR #294 functional Marvel rendering package.
2. 8c3f1b7b: C++ access-control build fix for backing promotion.
3. e7290a9b: move storage-image usage validation out of sampled-texture binding.

The package changes 35 files: 25 C++ implementation/header files, the compiler
gitlink and nine test files. Functional deltas include MME subtraction carry,
RenderArea push constants, raw global-pointer/SSBO binding, logical texture
extent matching, mutable backing promotion/retries, view and framebuffer
lifetime, full imageless format lists, backing-mip blit normalization and
nvhost AS FreeSpace map validation. They are not all texture-manager changes.

The shader compiler jump contains one commit and four files: the global-memory
lowering pass, shader_info.h, storage_buffer_range.h and translate_program.cpp.
It recognizes verified 8-byte aligned CBUF U64 pointers, classifies NVN sized
descriptors versus raw pointers and infers affine compute access extents. The
host can request up to 8 MiB for unknown raw-pointer ranges, clamped by Vulkan
limits and mapping availability; that is not evidence of an 8 MiB copy per
draw. Graphics raw descriptors do not use the compute extent inference.

All three historical workflows used ARM64 **DevReldebug**, NDK 27.3.13750724,
Clang 18.0.4, CMake 3.31.5, Gradle 8.2 and Java 17. The relevant Gradle/CMake
files are identical between good and slow. CMake sets RelWithDebInfo to -O1
before adding shader_recompiler and Skyline. These are not Release measurements.
Future builds preserve the existing ARM64 **DevRelease** workflow and its
Release configuration; no workflow edit is needed.

Current master pins compiler c10731c2d5ab00dbc115e651edbb88e221dcf9cc, which
preserves the dcb93886 pointer changes. RenderArea, SSBO range logic, promotion,
GetUsage, format lists, MME, FreeSpace and stricter matching remain, with later
extensions to mips, aliases and physical mapping. Later query/CBUF/TexMan
changes cannot explain the first slow historical build because it lacks them.

The original PerfDiag294 log records Turnip 24.0.99, Vulkan 1.3.280, Zelda 1.0.0
and Handheld mode. Last printed milestones include 2,048 rejected candidates,
2,048 rejections with an alternative reusable texture, eight new textures after
rejection, 16 readbacks/12 cumulative ms at that milestone, two promotions and
512 total new textures. These power-of-two logs are not final exact totals;
candidate rejection is not a cache miss, the eight objects are not the total,
and 12 ms is not per-frame cost. The earlier proprietary-driver 0.777.0 log with
Stable-60fps-v3 ends at the sampled/storage validation error and is not a slow
gameplay benchmark. Matching good/slow device profiling remains missing.

## Implemented optimization 1: render-pass cache hash contract

File: app/src/main/cpp/skyline/gpu/cache/renderpass_cache.cpp.

Owned RenderPassMetadata hashes the size of every resolveAttachments vector,
including an empty vector. The borrowed vk::RenderPassCreateInfo overload
previously omitted this count when pResolveAttachments was null. Equal
representations consequently hashed differently. Current RenderPassNode
subpasses do not populate pResolveAttachments, reaching the affected path.

The heterogeneous find can miss an existing entry. The fallback constructs
owned metadata vectors and try_emplace finds the existing owned key. This
normally avoids duplicate Vulkan render-pass creation but does unnecessary
metadata copies, allocations and hashing under the cache mutex.

The fix encodes a zero resolve count for the null pointer. Equality, key
ownership and Vulkan state are unchanged. Tests first failed on the actual
owned/borrowed hash-contract assertion, then passed after the two-line fix.

The host harness extracts production constructors/hash/equality and uses the
pinned Vulkan-Hpp and Boost dependencies. Its host comparator adapter forwards
either STL argument order to the production stored-key/borrowed-query overload,
matching the operation used by libc++ and avoiding unrelated implicit metadata
conversion by libstdc++. The cache benchmark mirrors GetRenderPass's find and
fallback try_emplace; it does not execute the full Android method or Vulkan.

## Implemented optimization 2: bounded buffer overlap fallback

File: app/src/main/cpp/skyline/gpu/buffer_manager.cpp.

Lookup's page-table fast path is unchanged. On fallback, lower_bound chooses
the last possible start address, then reverse traversal formerly continued
through all prior mappings even after their end was before the query start.

Live mappings are disjoint: FindOrCreateImpl inserts directly only for an
empty overlap set; otherwise it coalesces all overlaps, removes them and inserts
the union. InsertBuffer/DeleteBuffer are the only vector mutation sites.
Therefore once a reverse candidate ends at or before range.begin, all earlier
ones also end there or earlier. Stop at that point, retaining the existing
end-exclusive overlap comparison and the same result/resource-lock order.

The test extracts the actual Lookup and BufferLessThan bodies, using lightweight
range, buffer and page-table/lock facades. It compares overlap identities and
order to an independent full-scan oracle for 47,901 nonempty queries, uniform
and irregular disjoint maps, touching boundaries, gaps, multi-overlap ranges,
fast-hit bypass and a page-table candidate that does not cover the query.
Before the fix, semantics passed but the work-bound assertion failed; after
the fix, a tail one-overlap query reads two candidate ends and a gap after the
last mapping reads one, rather than 4,096 each. Real locks/traps/coalescing and
GPU memory transfers are not emulated or benchmarked by this fixture.

## Measurements and tests

Host: x86-64 Intel Xeon Platinum 8573C, GCC 13.3.0, C++20.
Benchmark builds: -O3 -DNDEBUG, without sanitizers. Five samples per variant,
baseline then optimized in the same environment; no device extrapolation or
statistical confidence interval is claimed. Buffer candidate reads are counted
by the fixture, so timing includes that instrumentation in both variants.

| Synthetic workload | Baseline median | Optimized median | Deterministic work metric |
|---|---:|---:|---|
| 200,000 warm no-resolve render-pass lookups | 66.378 ms | 28.920 ms | 200,000 metadata rebuilds to zero; cache size stays one |
| 30,000 forced buffer fallback queries near tail of 4,096 disjoint mappings | 341.684 ms | 1.067 ms | 122,775,000 candidate-end reads to 60,000; same overlaps |

Render-pass samples before: 66.378, 67.200, 66.154, 60.098, 68.646 ms.
After: 28.920, 28.140, 29.417, 28.212, 28.966 ms.
Buffer samples before: 354.823, 333.810, 336.245, 341.684, 345.744 ms.
After: 1.276, 1.009, 1.065, 1.092, 1.067 ms.

Passed locally:

- tests/gpu_renderpass_cache/run.py under ASan+UBSan: absent/present resolves,
  zero colors, non-null zero-count resolves, depth/input/preserve/dependencies,
  mixed subpasses, key ownership and changed-format/layout misses.
- tests/gpu_buffer_lookup/run.py under ASan+UBSan: 47,901 oracle cases and bounds.
- Existing tests/gpu_framebuffer_cache/run.py under UBSan: complete format-list
  ownership, expansion, heterogeneous hash/equality and fixed-view keys.
- git diff --check; production diff restricted to the two methods.

An independent read-only review found no Critical, Important or Minor issues
in the implementation and reran both new sanitizer suites successfully. It
explicitly did not certify Android compilation, real GPU/concurrency behavior,
device FPS or game compatibility; those remain the acceptance checks below.

LeakSanitizer is disabled with ASAN_OPTIONS=detect_leaks=0 because the host
cannot inspect process tasks. ASan and UBSan remain active. No local full APK
build, GPU validation-layer execution or gameplay compatibility test was done.
The new tests are optional host tests; no mandatory CI suite was added.

## Broad audit and opportunities left unchanged

Expected impact below is conditional on reaching the path, not a measured game
impact. Frequency and CPU/GPU time on Moto G100 are still required.

| Area | Finding / evidence | Expected opportunity | Risk and decision |
|---|---|---|---|
| CPU command recording | Render-pass cache contract mismatch above; current recorder reaches no-resolve path | Recurring metadata CPU work | Low; fixed, host benchmark only |
| BufferManager | Fallback scans all predecessor mappings | Large reduction per fallback; benefit depends on miss frequency | Low under current disjoint invariant; fixed and oracle-tested |
| Texture views / Vulkan driver | GetUsage queries format properties for foreign formats; same-format returns early | Avoid repeat immutable feature queries if frequent | Low/moderate; not implemented without call-frequency and thread/lifetime evidence |
| Framebuffer cache | Full format lists are necessary for legal views; regular hits use borrowed keys | Keep full semantics; profile hashing and GetUsage rather than remove lists | Medium; unchanged |
| Descriptors | Current allocator reuses slots and resets inactive pools; layout/push limits have correctness requirements | Profile slot scans and pool resets before changing | Medium/high; unchanged |
| Draw preparation | No-op quick bind purges views before zero-use return | Only saves work if no later descriptor-using sync in the execution tag | Low/moderate, small conditional benefit; deferred |
| Index conversion | Indexed quads regenerate converted indices on refresh | Conditional repeated conversion/allocation | Medium/high; reuse must include identity, generation, offsets/count/type and lifetime |
| Shader/pipeline variants | New PackedPipelineState translates/emits active shader stages; compilation uses a lock | Mostly loading/new-variant costs; shader compilation is not every draw | High for naive hash-only reuse because CBUF/texture/compare/runtime state matters; unchanged |
| Pipeline construction | New pipelines create descriptor/pipeline layouts and a compatibility render pass | New-pipeline creation cost | Medium, complete cache keys and shared lifetime required; unchanged |
| Buffer coalescing | Repeated DeleteBuffer performs vector find+erase | Conditional bulk-removal CPU work | Medium lifetime/invalidation-order risk; unmeasured, unchanged |
| Direct imported-buffer shadows | Activation resizes/zeros, deactivation clears/shrinks | Conditional allocator churn versus RAM retention | Medium memory-policy tradeoff; unchanged |
| TextureManager | Physical overlap indices and mapping checks preserve coherence; new overlaps can cause existing readback | Needs exact lookup/allocation/readback timings; counting aliases alone is insufficient | High if bypassing writer authority; unchanged |
| Submissions / barriers / waits | Executor chains predecessor submission and fence dependencies; megabuffer polls chunks after active exhaustion | Need frequency and wait-duration profiles | High; do not remove ordering or use global waits |
| Scheduler / logger / other paths | Static reading alone cannot establish contribution to this Zelda workload | Separate counters/trace evidence required | High for scheduler changes; no speculative tweak or log spam |

Potential correctness concerns outside the changed methods were flagged for
separate reproduction, not modified or claimed to explain FPS: split shader
fallback bounds, scheduler timeslice arithmetic, and render-pass attachment
preservation/equality conditions. They require dedicated correctness work.

## Moto G100 acceptance and next profiling

Compare base master and this branch as ARM64 **DevRelease** with the same
driver, game/update/mod, save/scene, Handheld setting, Free Guest/readback
options, log level, warmed independent caches and comparable temperature.
Use base to optimized to base runs. Record frametimes, sustained FPS and scene
identity separately from initial pipeline compilation.

Smoke-test Marvel Cosmic Invasion rendering, Zelda visuals and transitions,
Dragon Ball FighterZ, Burnout, Bulletstorm, Demon Slayer and representative UE4
titles. Existing coherency/sequencing work and existing PRs are preserved.

If the historical regression remains, collect bounded aggregate measurements
of CPU draw preparation/recording, fallback lookup frequency, metadata rebuilds,
submissions/retries, fence waits, effective SSBO ranges/bytes synchronized,
GetUsage queries, pipeline misses/compilation, draws/dispatches and GPU time
when available. Prefer existing traces; do not print per-draw logs or add a
large capture system. Choose one subsequent experiment from measured dominant
work, rather than revert PR #294, MME, RenderArea, aliases or compiler features.

No FPS recovery, resolution of the historical regression or cross-game
compatibility result is asserted by the host measurements in this report.
