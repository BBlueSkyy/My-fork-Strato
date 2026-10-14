# PR #317: Maxwell occlusion-query audit

Audit date: 2026-10-14. Original head: `81e7439b2275d9142c1ac79f0d2961bb58f82bb9`.
Base/master: `3e8689f6c99c4678f473178bd0aa99f8528a615e`, already an ancestor of the PR.
The audit preserves master texture alias, mip-view, pitch-linear and executor changes.
No merge into master is performed.

## Proven defects and corrections

1. **Deferred reports could execute after their consumer.** A draw, report, then
   a command reading its destination observed the old contents: resolving only at
   the end of the render pass put the report after that read. Report now ends the
   active Vulkan pass and publishes the result at its command-stream position.
   This is a render-pass boundary, not a command submission or CPU wait.
2. **Accumulator resources could die during deferred recording/GPU execution.**
   `ScheduleResolve` captured the owning counter through `this`. Maxwell is
   destroyed before its executor drains. The accumulator is now shared, captured
   by value and attached to every consuming fence, including empty reports with
   no query bank.
3. **Empty resets lacked a dependency before `fillBuffer`.** Prior compute
   accesses and report transfer reads could overlap the reset. Khronos sync
   validation reproduced WRITE_AFTER_READ hazards. A transfer/compute dependency
   now precedes the fill.
4. **Report destinations lacked general memory dependencies.** Overlapping
   reports, replacement of a predicate used by prior work, and later DMA readers
   require ordering and visibility. Pre/post buffer barriers cover only the
   destination's actual offset and 4-byte or 16-byte report range. Timestamp
   writes remain ordered before the result write.
5. **Equal/NotEqual truncated operands.** Both CPU and GPU paths compared only
   the low 32 bits. They now compare 64 bits at the guest's base and base+16.
   The compute shader uses two scalar words per operand, requiring no shaderInt64.
6. **DRAW_TEXTURE discarded prepared predicates and did not start counting.**
   Its helper draw is now conditionally bracketed and receives the same sample
   counter hooks as programmable draws. Pipeline notification precedes helper
   rendering; unrelated helper work cannot accidentally inherit an active query.
7. **Predicate caches could survive input writes or mapping changes.** The cache
   now keys input buffer identity, offset and a generation advanced on sequenced
   writes. Both inputs block sequenced CPU backing writes, so later I2M updates
   cannot overtake deferred reads. Flush purges mapping caches. Cached writable
   graphics bindings also require invalidation after each potentially overlapping
   draw, independently of whether descriptors were rebound.
8. **Predicate scratch/descriptors could die before GPU completion.** A shared
   resource bundle is retained by comparison callbacks and every consuming fence.
   Draw parameters stored with `EmplaceUntracked` retain only a trivial binding;
   shared ownership is not leaked through an allocator that skips destructors.
9. **Texture retries could retain a predicate from the old submission.** A
   requested condition is kept separately from prepared GPU state. Preparation
   occurs inside each retried draw/clear after texture preparation succeeds.
10. **Hosts without EXT rendered GPU conditions unconditionally.** The fallback
    now reads coherent operands and honors the condition. Only a GPU-dirty input
    requiring current work invokes submission; clean CPU values do not submit.
    The buffer is retained and its context lock reacquired if submission releases
    executor attachments before the read resumes.

Unused pre-render-pass-end hook queues, unused hook fields and the unbounded
historical query-address set were removed.

## Original 17-file audit coverage

Paths below are relative to `app/src/main/`.

| File | Audit scope/result |
|---|---|
| `cpp/skyline/gpu.cpp` | Optional conditional feature structure in device chain; no mandatory extension added. |
| `cpp/skyline/gpu/interconnect/command_executor.cpp` | End-query callbacks precede render-pass/subpass exit; hook starts occur after entry. Current master submission/texture retry logic retained. |
| `cpp/skyline/gpu/interconnect/command_executor.h` | Hook lifetime/API; removed unused queues/hooks. |
| `cpp/skyline/gpu/interconnect/fermi_2d.cpp` | Pipeline notification before helper blit pauses Maxwell queries. |
| `cpp/skyline/gpu/interconnect/maxwell_3d/maxwell_3d.cpp` | Inline/direct/indirect/DRAW_TEXTURE counting, clear exclusion, predicates, retries, fallback and flush order. |
| `cpp/skyline/gpu/interconnect/maxwell_3d/maxwell_3d.h` | Predicate bundle ownership, trivial untracked draw bindings and cache identity. |
| `cpp/skyline/gpu/interconnect/maxwell_3d/queries.cpp` | Begin/end, segment accumulation, report/reset order, pool acquisition, fences, barriers and lifetime. |
| `cpp/skyline/gpu/interconnect/maxwell_3d/queries.h` | Persistent state across executions, no allocator pointers surviving execution changes. |
| `cpp/skyline/gpu/memory_manager.cpp` | Conditional buffer usage enabled only with the optional feature, for allocated and imported buffers. |
| `cpp/skyline/gpu/shaders/helper_shaders.cpp` | Descriptor ranges, push constants, pipeline binding and compute/transfer visibility. |
| `cpp/skyline/gpu/shaders/helper_shaders.h` | CPU/shader layout agreement; no mandatory 64-bit shader arithmetic. |
| `cpp/skyline/gpu/trait_manager.cpp` | Precise count feature negotiation and optional conditional feature unlinking. |
| `cpp/skyline/gpu/trait_manager.h` | Optional capabilities default false; chain matches device creation. |
| `cpp/skyline/soc/gm20b/engines/maxwell_3d.cpp` | Register snapshot ordering, deferred draw flush, reports and conditional dispatch. |
| `cpp/skyline/soc/gm20b/engines/maxwell_3d.h` | sampleCounterEnable register linkage and conditional check interface. |
| `shaders/conditional_compare.comp` | Full-word equality, zero/one predicate and std430 offsets. |
| `shaders/query_resolve.comp` | Carry, segmented accumulation and modulo-64-bit wrapping; compiled and exercised unchanged. |

Additional buffer-generation and writable-binding tracking changes are necessary
for predicate cache correctness; they do not change texture ownership or the
existing buffer megabuffer sequence-number semantics.

## Counter and pool invariants

- Counting follows `sampleCounterEnable`, not the presence of report requests.
  Disabling pauses; enabling resumes with another segment. Reset discards prior
  pending samples and logically clears the persistent accumulator.
- Every segment is begun/ended within one Vulkan subpass. Clear/helper work pauses
  counting; DRAW_TEXTURE is a guest raster operation and participates when enabled.
- A report preserves the counter; subsequent reports remain cumulative until
  explicit reset. A 32-bit report writes only four bytes. A timestamp report writes
  eight counter bytes followed by eight timestamp bytes.
- A bank has 1024 indices. Overflow acquires another bank. Reuse requires the
  previous fence to have completed; unavailable banks allocate without a CPU wait.
- Flush ends segments and accumulates them before execution state rotates. The
  accumulator persists across submissions. The existing queue/initial executor
  barriers order cross-submission GPU accesses.
- Query-copy WAIT is a GPU query-result dependency, not a CPU readback. The normal
  extension path introduces no explicit Submit or CPU wait for report resolution.

## References checked

Pinned source was read from the following projects rather than inferred from
names or current branch summaries:

- [Eden official mirror, 14da4482](https://github.com/eden-emulator/mirror/tree/14da4482a4668928bad43bb67ef12f96b9c9d972/src/video_core):
  Maxwell `ProcessQueryCondition`, Vulkan `DrawTexture`, query-cache counter
  start/pause/reset/report and conditional resolve shader.
- [Yuzu historical mirror, 7ffac53c](https://github.com/yuzu-emu-mirror/yuzu-mainline/tree/7ffac53c9e67fa18d067114a8eb3e77c74bba68f/src/video_core):
  the corresponding engine, rasterizer, query cache and conditional shader.
- [Ryujinx archive, 534f9250](https://github.com/kotx/Ryujinx/tree/534f92506bf8c6aba29bb86917bf10954a360d8e/src):
  `ConditionalRendering.CounterCompare` and `SemaphoreUpdater.ReportCounter`.
  This is an archived source snapshot, not a claim about a maintained upstream.
- [Vulkan query begin](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBeginQuery.html),
  [query-result copy](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyQueryPoolResults.html),
  [conditional rendering](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBeginConditionalRenderingEXT.html).

## Validation and remaining limits

Reproduction commands:

```sh
python3 tests/gpu_queries/run.py
python3 tests/gpu_conditional/run.py
python3 tests/gpu_draw_texture/run.py
python3 tests/gpu_queries/compile_vulkan.py --required
python3 tests/gpu_queries/run_vulkan.py --required
python3 tests/gpu_framebuffer_cache/run.py
bash tests/gpu/run_pitch_linear_tests.sh
git diff --check
```

- Production query/conditional methods are compiled through host interfaces under
  ASan/UBSan: 15 query, 20 conditional and 16 DRAW_TEXTURE cases pass. The query
  tests cover optional precise-feature flags, rollover, in-flight banks, cumulative results,
  carry, timestamp bounds, reset, consumer ordering and destruction before recording.
- Exact query classes and helper pipelines compile against pinned Vulkan-Hpp 261.
  This syntax check substitutes Android/engine interfaces; it is not a full APK build.
- Both compute shaders compile for Vulkan 1.1 and pass `spirv-val`. Mesa lavapipe
  with Khronos synchronization validation exercises actual query reset/begin/end,
  1024-result copy/resolve and fence reuse, full-word predicates and real triangle
  occlusion with conditional rendering enabled and disabled. Current runs have
  zero validation errors/hazards. Removing the reset dependency reproduces the
  original hazards; removing report visibility reproduces DMA RAW hazards.
- Existing DRAW_TEXTURE, framebuffer, mip-view and pitched-copy/BC decode checks
  pass. `tests/gpu_backing_promotion/run.py` fails to compile identically on the
  untouched master: its host fixture lacks pitch-linear helpers and has stale
  dirty-state/Extent3D declarations. It was not repaired in this query change.
- The PR workflow runs the new host regressions before Android assembly. A full
  Android SDK/NDK build was not performed locally. CI is started at publication;
  its result is intentionally not followed in this session.

**Device/performance gate remains open.** No Android device, game execution or
Bulletstorm FPS measurement was available. The report-ordering fix can increase
render-pass boundaries versus the original PR's deferred reports; no causal FPS
claim or performance improvement is asserted. GPU accumulation, asynchronous pool
reuse and unchanged-input predicate reuse remain. Test Bulletstorm on the same
scene, driver, resolution and warmed shader cache, as well as representative
occlusion/conditional-heavy games, before merge.

**Hardware accuracy limits are explicit.** Without `occlusionQueryPrecise`, Vulkan
only guarantees visibility-style query behavior, not exact sample counts. NonZero
keeps the existing low-u32 behavior: Ryujinx tests u64!=0, while examined Yuzu/Eden
paths disagree among themselves about the two words. No conclusive Maxwell
hardware evidence justified changing this behavior during the audit. Equality and
difference are corrected to 64 bits; this unresolved NonZero question is not
presented as solved.

The branch needs a successful Android build and device regression/performance
validation before being recommended for merge.
