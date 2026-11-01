# Coherent resource hotpaths on current master

Base: `fc8eb5196c198b281dce6bb3d9a9126b11ac5d4a` (merged #409).
Branch: `perf/coherent-resource-hotpaths-20261101`.
Compiler gitlink remains `a668a31370f1739fdbf5375cad3e171d0fbd0601`.

This is a bounded production optimization candidate, not a claim that Zelda now
runs at normal speed or that all memory-coherence problems are solved. No Moto
G100, Android GPU governor measurements, GPU timestamps, game simulation counters
or title gameplay results are available in this environment.

## Changes and evidence

| Mechanism | Proven redundant work | Change | Scope of proof |
|---|---|---|---|
| Render-pass borrowed lookup | Owned metadata hashes an absent resolve list's length as zero; borrowed queries omitted that value. Equal keys consequently hashed differently. | Hash zero consistently, preserving the cache's existing mutex and ownership. | Actual production keys/hash/equality, CPU benchmark and native Vulkan handles. |
| Depth reference equality | Borrowed equality used `&&`, accepting a changed attachment **or** layout if the other field matched. | Reject when either field differs (`||`). | RED/GREEN direct equality regression, all stored attachment/subpass/dependency field differences, native depth/stencil formats. |
| Buffer overlap lookup | After lower_bound, the fallback scanned every earlier mapping even after reaching a predecessor ending before the requested range. | Stop there: live entries are disjoint because FindOrCreate coalesces overlaps and removes replaced entries. | 47,901 full-scan oracle cases, identical result/context/descending order, production insertion/deletion and serialized replacement schedules. |
| Texture layout no-op | TransitionLayout waited for GPU completion before checking whether the layout already matched. | After **WaitOnBacking**, return on equality and retain the resource's existing cycle. Actual transitions are unchanged. | Production backing/wait/transition bodies, pending producer and native upload/copy/readback, real missing-backing publication, failure/retry and retained lifetime. |

The two CPU changes reuse [Draft #401](https://github.com/BBlueSkyy/My-fork-Strato/pull/401)
(`9694ca58` and `ccc055d8`), with added safety coverage. Its prior device result
showed no perceptible Zelda FPS recovery; that negative result remains valid.
The no-op transition change selectively reuses the mechanism of #400's
`9babcf1a`; it does not import that branch's regional manager or other changes.
[Draft #405](https://github.com/BBlueSkyy/My-fork-Strato/pull/405) and
[Draft #406](https://github.com/BBlueSkyy/My-fork-Strato/pull/406) remain independent.

## CPU measurements

Same x86-64 host, GCC, `-O3 -DNDEBUG`, five runs per unchanged fixture.
The functional assertions remain enabled in the fixtures. Before/after checksums
and result cardinality agree. No Vulkan driver execution is timed here.

| Fixture | Master median | Candidate median | Work count |
|---|---:|---:|---|
| 200,000 warm render-pass queries | 55.609 ms | 29.870 ms | metadata reconstructions: 200,000 → 0 |
| 30,000 forced fallback buffer lookups over 4,096 live mappings | 100.245 ms | 0.941 ms | candidate end reads: 122,775,000 → 60,000 |

Render-pass samples before: 55.221, 55.598, 55.618, 55.609, 55.790 ms;
after: 27.086, 28.593, 44.479, 29.870, 31.188 ms. The visible timing variation
limits precision; the eliminated operation count is deterministic. This is not
an Android frame-time reduction, and the buffer workload deliberately forces the
fallback rather than the normal page-table hit. The earlier experiments were
reused; no historical game/compiler A–B–A experiment was repeated.

## Synchronization and backing contract

Storage-image preparation in `pipeline.inc` calls TransitionLayout(General)
repeatedly. Matching layouts do not need a new transition. They still need their
producer's submission, barriers and lifetime. Those remain in SubmitInternal:
WaitSubmit on the predecessor, ChainCycle, resource retention and full barriers
before/after host synchronization. Presentation's source semaphore path and CPU
readbacks retain their own waits. No fence, semaphore, barrier or command is
removed from a real data operation.

WaitOnBacking **cannot** be skipped. Its condition-variable wait may release the
texture lock while a replacement is published, and that replacement can have a
different layout. The new test starts with an absent backing whose stale layout
already equals General, checks that a wakeup with no valid image does not finish,
then publishes an Undefined replacement through production SwapBacking. A real
General transition must follow. The fixture uses a tracked condition-variable
adapter around the production wait body; Android locking remains untested.

The native test first submits an upload retained across a same-layout no-op and
reads it in General using the exact executor RecordFullBarrier. Only the requested
readback's fence is waited on; the producer's CPU wait count stays zero. It then
checks real layout changes, image-to-image copy, readback bytes, retained objects,
and allocation failure/retry. The initial test transport lacked incoming
consumer barriers and Vulkan synchronization validation rejected it. The fixture
was corrected to use the actual executor barrier; product barriers were not
weakened or changed to make validation pass.

The native harness now extracts production WaitOnBacking, WaitOnFence and
TransitionLayout instead of the former handwritten transition. Copy transport,
allocator/scheduler/trap interfaces and portions of the resource facade remain
test boundaries; passing lavapipe validation is not a full Android-driver proof.

## Cache and buffer safety

The render-pass key tests distinguish formats, flags, sample counts, load/store
and stencil operations, initial/final layouts, input/color/resolve/depth
references, preserves, bind point and all dependency fields. Equality checks
exercise different depth fields directly, independent of hash bucket collisions.
Native cache tests create actual render passes for D16, D32, D24S8 and D32S8,
keep different depth layouts/attachments distinct and reuse handles concurrently.
The cache continues to own handles until destruction within the device lifetime.

Framebuffer attachment usage and view-format lists remain in FramebufferCache,
not the render-pass key. Its existing production key suite validates format-list
ownership and fixed-view keys. Image/view generation retirement is unchanged.
No new support for render-pass pNext extensions or creation flags is claimed;
current production construction supplies neither.

BufferManager remains caller-serialized. The new tests execute actual Lookup,
InsertBuffer and DeleteBuffer with range/table/lock-boundary facades. They cover
partial overlaps, gaps, table hits, invalidated retained aliases, replaced maps
and concurrent callers under the required manager lock. They do not establish
whole-emulator/NCE lock correctness. Coalescence, dirty tracking, delegate
relocation, data copying and buffer lifetime algorithms are unchanged; the SSBO
host suite separately checks partial/split mappings, aliases and relocation.

## Wider audit: measured history versus current hypotheses

- Buffer SynchronizeHost still waits when required, arms CPU write protection and
  copies dirty mirrors. SynchronizeGuest still waits before GPU→CPU publication.
  No range-based dirty system or equality-based copy suppression was added:
  page traps and aliases need stronger evidence before changing their authority.
- Megabuffer allocations still chain their consumer cycles. Texture aliases and
  promotion retain existing sequencing, views and locks. No global GPU waits or
  new resource cache invalidation policy were added.
- Command submission and presentation retain all semaphores/fences. Historical
  #404 Host1x waits overlap GPFIFO/recording costs and cannot be added as a frame
  budget. They do not prove GPU saturation, CPU-only parsing cost or governor
  ramp-up as the current cause after #409.
- Force Maximum GPU Clocks controls adrenotools turbo in ExecutionWaiterThread;
  the inspected path does not alter guest clocks. The FPS overlay derives from
  presentation timing and can use desired presentation timestamps. Neither is
  a guest simulation-speed measurement. Queue latency, governor behavior,
  scheduling and game timing still require device measurement; no causal claim
  about Zelda's slowdown is made.

Eden comparison: [#4490](https://git.eden-emu.dev/eden-emu/eden/pulls/4490)
flushes owned GPU buffer bytes before NCE invalidation so stale guest pages do not
replace GPU ownership. [#4531](https://git.eden-emu.dev/eden-emu/eden/pulls/4531)
changes locking/clean-range handling. [#4584](https://git.eden-emu.dev/eden-emu/eden/pulls/4584)
reverts that approach after a DKCR slowdown. Proposed follow-up
[#4605](https://git.eden-emu.dev/eden-emu/eden/pulls/4605) discusses waiting on an
already recorded asynchronous download before synchronous fallback; it is not
validation for Strato. Strato's hacks-OFF buffer trap already waits/retries and
SynchronizeGuest precedes CPU-dirty publication. An unconditional Eden-style
queue drain would duplicate work and require an architectural change, so none
was copied.

Ryujinx's [BufferModifiedRangeList](https://git.axenov.dev/Museum/ryujinx/src/commit/50bdda5baafcdca2773b38e5896c611b7bcc3112/src/Ryujinx.Graphics.Gpu/Memory/BufferModifiedRangeList.cs)
tracks modified byte ranges and sync generations, waits for eligible prior
writes, and retains newer ranges. This supports the ownership/generation design
principle, not a direct port to Strato's coarse dirty flags. No Eden/Ryujinx code
was imported.

## Validation and an existing Free Guest limitation

Local commands: cache/buffer runners with ASan+UBSan (`ASAN_OPTIONS=detect_leaks=0`);
`tests/gpu_renderpass_cache/run_vulkan.py`; native
`tests/gpu_backing_promotion/run.py` with Clang 18, real lavapipe and synchronization
validation; `tests/gpu_submission/run.py` with ASan+UBSan;
`tests/gpu_framebuffer_cache/run.py`; `tests/gpu_sampled_layout/run.py`;
`tests/ssbo_bounded/run.py --compiler app/libraries/shader-compiler`; and compiler
`tests/structured_ranges/run.py --deps app/libraries` with ASan+UBSan.
LeakSanitizer alone is disabled because this host cannot inspect process tasks.

The old native Free Guest bridge fixture failed on **unmodified master too**.
It omitted the explicit requested layout while choosing an evicted first member
as the default reference, and expected random padding to survive eviction even
though only image texels can reconstruct it. The positive bridge now uses a
proved dense writer and the explicit layout, retaining assertions for rejected
missing references, restored bytes and writer generations. This is a fixture
correction, not a production padding fix. Master still lacks #400's guest-discard
coverage proof: enabling Free Guest on padded allocations remains a known risk.
That original failure must not be interpreted as proof that padded eviction is
safe in this candidate. Four clean-state fixture command buffers also needed
explicit cancellation: they deliberately recorded/submitted no GPU work, but
left unsubmitted facade cycles in the scheduler drain. Only test lifecycle was
corrected. With the same fixture corrections, both unmodified master and the
candidate pass the full native backing suite; the candidate additionally passes
ASan+UBSan and its new layout tests with zero synchronization validation errors.
#400's separate authority probe passed 0/1,024 stale shared texels with Free Guest
OFF/ON and both format-list modes, including donor/callback/failed-operation
cases; its lock probes passed. Those tests run against #400, not this master's
regional implementation, and do not certify padded eviction in this candidate.
Its production regional implementation is not added here.

An independent read-only review found no Critical/Important product defect. It
identified the initial native coverage gap (readback after a real transition)
and temporary test phase logging. The direct General consumer/readback case
closes the explicit producer-wait gap, and all phase diagnostics were removed.
The reviewer did not certify Android performance, complete manager lock ownership
or native results it did not independently run.

No temporary product logging, tracking option, game/hash special case or clock
change is included. Android workflow/build configuration remains unchanged:
ARM64 `assembleDevRelease`, not RelDebug. Build success and APK existence must be
recorded from the actual completed run, separately from host results.

## Moto G100 acceptance still pending

Use Zelda 1.0.0, Turnip 24.0.99, the same save/scene/resolution and warm shader
cache. Start with Readbacks+ **OFF**, Force Maximum GPU Clocks **OFF**, Free Guest
**OFF**, and no FPS/resolution cheat. Keep identical thermal conditions and log
level. Compare current master including #409 against the candidate. Record real
wall duration, presented frames/frametimes, complete effects/objects/shadows/UI,
and an identical game-action duration independently of FPS. Repeat the clock
ON/OFF comparison only to isolate the remaining governor/timing question; turbo
is not an acceptance requirement or proposed fix.

Then test Marvel Cosmic Invasion gameplay (no magenta, complete effects),
SpongeBob menu/gameplay and appropriate Sparking ZERO/FighterZ scenes. Check
sustained performance, frame pacing, crashes and graphics. The known ~30 FPS SSBO
experimental APK is historical evidence, not validation of this new candidate.
