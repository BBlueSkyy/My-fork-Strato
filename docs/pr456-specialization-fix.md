# PR456 — recovered specialization correction

This continues PR454 from `d951ca35`; PR454 and diagnostic PR455 (`3087d5a5`)
are untouched. The first durable recovery is commit `fcd223ef` on the dedicated
branch, including the unfinished implementation and the failing equivalent-handle
regression. The original code, fixtures and red/green results are preserved in
`docs/checkpoints/pr454-session-20261115/` and the local checkpoint branches.

## Findings and corrections

1. **Stale specialized pipelines:** graphics map, current pipeline, populated
   transition cache and compute map reused compiler metadata after a TIC changed.
   The four original failures were reproduced. Pipelines now retain normalized
   snapshots of compiler dependencies and each packed key owns its variants.
2. **Handle origin matters:** comparing a previously queried TIC index misses
   changes in the CBUF handle. Queries now resolve their current primary and
   secondary CBUF words, including shifts and TSC bits, before reading metadata.
   Genuine CBUF words folded into code/control flow remain strict dependencies.
3. **Raw handles are not shader constants:** the recovered naive implementation
   failed two assertions because equivalent textures caused unnecessary variants.
   The already-started `Environment.QueryTexture(source,kind)` interface separates
   metadata lookups from scalar constant reads. Equal queried metadata reuses the
   same shader/pipeline even when raw handles change. This is an interface for
   cache dependencies, not a shader transformation or a claimed compiler defect.
4. **Clean draws:** `PipelineState::Refresh` revalidates the dependencies even
   when register dirtiness skips Flush. Related type/int/compare queries share
   one handle read. Unqueried uniforms (including Kirby's live P0 source) do not
   cause compilation. No whole-pool hashes, per-draw frontend translation or hot
   snapshot allocations were added. Skipping reads based only on a channel or
   sequenced-write generation would miss unsequenced guest writes, so it is not
   used as a false fast path.
5. **Descriptor fast path:** matching layouts alone allowed a fresh specialization
   to inherit stale sampler/integer descriptors. `CheckBindingMatch` now also
   requires matching specialization. Four explicit failures in the actual
   PrepareDraw descriptor-selection lambda were reproduced and corrected.
6. **Safe compile reads:** unbound compile CBUFs return the existing zero-handle
   policy, without touching a null BufferView. A bound but too-short word fails
   explicitly before memory access; no speculative value is invented. Four
   unsafe-access assertions were reproduced and corrected. Pending GPU writes
   still unwind via the existing retryable read, not a mid-draw submission.
7. **Lifetime and preloads:** owning vectors retain old Pipeline objects while
   raw references/commands can still use them. V5 cache files retain their layout;
   dependency origins are rebuilt by translation, rather than treating every
   saved CBUF value as a baked scalar. Duplicate preloads stay owned until the
   assembler drains. The preload regression caught early resource destruction
   on corruption and on another exception class; both paths now drain before
   owner destruction and before rethrow/unregistration.

The obsolete interim `PipelineStateBundle::GetSpecialization` implementation was
removed from the current tree because raw saved CBUF values do not identify their
use. Its full source remains in the recovery commit; no cache-file data or
functional checkpoint was discarded.

## Kirby evidence retained

See [the original replay](pr454-shader-replay.md) and
[the byte-identity verification](pr456-query-replay.md). Both actual captured
fragment binaries retain all six samples. `CBUF2[0x30]` is sampled only on the
true edge of the live `CBUF3[4] != 0` unordered comparison. The observed zero
states skip it. The compiler interface checkpoint produces **byte-identical
SPIR-V** to the original pinned compiler under the same declared offline profile.
No shader operation, predicate, guest CBUF value or game-specific path is patched.

The TIC0 poison bytes and unmapped image address explain why eager CPU descriptor
preparation can encounter an inactive resource. They do not prove Maxwell's
result for an invalid sample that actually executes. The existing sampled
`nullDescriptor` compatibility policy is retained; reserved storage images and
unsupported hosts remain explicit errors. Raw zero is not globally null: valid
TIC0 continues to be a valid texture.

## Executed gates

- Original checkpoint: 1,114 checks preserved; expanded TIC matrix: **1,117/1,117** checks
  with ASan/UBSan. No earlier red output was relabeled as success.
- **52/52 cache/lifetime checks** and the normalized snapshot helper pass with ASan/UBSan: original four misses,
  equivalent handle reuse, unchanged old TIC, changed TSC bits with the same TIC,
  split handles, scalar folding, clean draws, unbind and short views.
- Real Vulkan: seven sampled dimensionalities and storage write/read produce
  64 zero words with no validation errors. Separate real VkPipeline owners are
  exercised through the actual manager/cache methods while a timeline semaphore
  holds the old submission/fence pending across new variants.
- Actual preload constructor with ASan/UBSan: valid deduplication/distinct variants,
  corrupt tail and non-cache exception; no resources destroyed before drain.
- Actual compiler TexturePass: 16 bound/bindless target cases with UBSan.
- Actual Environment default/override contract: four query kinds, source shifts,
  OR and duplicate-read compatibility with UBSan.
- Captured Maxwell replay: both modules validate and are byte-identical to the
  original results. Their P0 CFG proofs remain valid. Original artifacts were
  not rebuilt or overwritten; the changed interface was tested separately.
- Existing null-storage, sampled-layout/alias, multirange, framebuffer and
  submission results remain recorded. Mandatory CI additionally compiles the
  complete Android renderer and runs the existing compiler regression gates.

For CPU cost, the host fixture alternates 20,000 equivalent handles through the
actual dirty refresh and matching functions: zero compilations, zero Flush calls,
one CBUF word per draw for the three metadata queries. Its nanosecond timings
measure facades and are **not Moto G100 CPU/frametime measurements**. Real buffer
locking, device driver costs and workload-dependent variant counts require phone
validation. Variant destruction is deferred by ownership rather than a new GC.

## Remaining limits and merge decision

- Arbitrary same-sequence raw CPU changes to TIC/TSC contents without the normal
  descriptor invalidation boundary are an existing descriptor-coherence problem.
  This correction does not claim to fix every descriptor replacement path or
  add descriptor bytes/nullness to shader specialization.
- The two earlier optional baseline failures remain documented: the pitch fixture
  is stale, and backing-promotion aborts on alias-layout reconciliation. They
  were reproduced on the original PR and are not masked or declared passed.
- No new APK has been tested on Moto G100, and other-game regressions/FPS are not
  certified by a host harness. Test Kirby, MK8, SMO, BOTW, RE5, buffer/storage users
  and both driver families as applicable; an unsupported nullDescriptor path
  remains a fail-closed case, not a promise of compatibility.

The correction is grounded for specialized reuse and the observed eager-binding
failure. Keep PR456 Draft and do not merge until the Android build and on-device
compatibility/performance checks are reviewed. A passing release build does not
establish Maxwell behavior for a genuinely executed invalid texture access.

Detailed final local outputs and preserved failing controls: [validation archive](checkpoints/pr456-validation-20261115/results.txt). Android CI and APK verification are recorded separately after completion.
