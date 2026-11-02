# PAC-MAN Re-PAC — Stage 12: synthetic index-binding contract

## Verified graphics-state-only result

Starting HEAD: `2ca42b938d2a800e8dc20675855460cf91980f27`, Draft #414.
The user confirmed that **mode 17 also eliminates the triangle**.

New capture/log identities: `libfile_3611706756808191a89862a0fec6c76e` and
`libfile_bbf22965c7608191a886ec7ff252f4b3`.
ZIP SHA-256 `4ee2957f632a1632a912ccb7b6100c06e27e78721b2a5d25c1c0b34017c0587f`;
log SHA-256 `6624562b13b382a795db0a8dff30fe56bf26a21d4c48dbd5c04a2aff728a35e8`.

Verified: mode 17, threshold 256, 60 presents, 244 candidates, 122 refreshes,
zero forced submits, 61 logged natural submits, 1337 events, zero dropped.
Every selected callback pair reports `callbacks_selected=1` and unchanged
node/pending/submission/renderpass/cycle/tag values. The same four textures
have 364 complete sync pairs, all GpuDirty with no inline upload.

This establishes sufficiency of the graphics callback in this run, without
query work, compute invalidation, channel-sequence advancement or added queue
submissions. It still does not identify the individual stale graphics state.

## Concrete source defect found

`Maxwell3D::Draw()` calls `PrepareDraw()` with the original guest indexed flag.
For a nonindexed quad, it later calls `StateUpdateBuilder::SetIndexBuffer()`
with `quadConversionBuffer` and rewrites the operation as an indexed triangle
draw. `DrawWithInlineIndex()` similarly overwrites the prepared guest binding
with `inlineIndexBuffer`.

Neither path invalidates `ActiveState::indexBuffer`. For the next guest indexed
draw with unchanged registers, size and megabuffer binding,
`IndexBufferState::Refresh()` returns false without issuing another bind. Vulkan
therefore retains the synthetic binding rather than the cached guest binding.
This violates the index-cache contract independently of any particular title.

Graphics-wide `MarkAllDirty()` invalidates this cache and would mask that defect.
Slots 1/3/4 are logged with guest topology 7 (Quads), indexed=true and count=6,
but **the existing trace is emitted after quad conversion**. It cannot distinguish
an original nonindexed four-vertex quad from an originally indexed operation.
The source defect is concrete; **its causal role in PAC-MAN is not yet proven**.

Other audit findings are not bundled into this test: descriptor compatibility,
sampler freshness and viewport depth-sign tracking remain distinct possibilities.

## Smallest targeted validation

Mode 18, `ENABLE_EXECUTOR_INDEX_REBIND_TEST`, tests only invalidating the guest
index state immediately after either synthetic index override. It does not use
postprocess slot selection or hashes/formats/dimensions. All other graphics state,
descriptors, pools, callbacks, query state, texture synchronization, cycles and
threshold policy retain their normal behavior.

The narrow operation is `ActiveState::InvalidateIndexBuffer()` →
`indexBuffer.MarkDirty(false)`. False retains the mapped-buffer cache and merely
requires the next indexed preparation to emit its binding again. This is initially
**opt-in diagnostic behavior**, not a published definitive PAC-MAN fix.

- [x] Verify mode-17 activation, counters and user-observed visual success.
- [x] Reproduce guest binding → synthetic override → unchanged guest indexed
  draw with actual index-cache and dirty-tracking code. Verify baseline failure.
- [x] Add the one-state invalidation and mode-18 gating at both real override
  sites. Preserve all prior modes and default threshold 256.
- [x] Record original guest counts/indexed state before conversion, source kind,
  shader hashes, override submission and next actual guest index-binding command.
  Keep counters separate from forced submissions.
- [x] Run relevant host safety checks and independent review; publish only the
  diagnosis/probe on the existing Draft, request ARM64 DevRelease, no Actions monitoring.

Expected metadata: `executor_experiment_mode=18`,
`executor_experiment_draws=0`, `executor_callback_refreshes=0`, plus separate
`executor_index_invalidations` and `executor_index_rebinds`. A zero invalidation
count means the source path was not exercised and makes the test inconclusive.
Trace original nonindexed quad and inline overrides separately; identify rebinds
within the same submission, since a naturally new command buffer already
requires ordinary state rebuilding. Logging the presence of a binding command
does not by itself verify its final GPU bytes.

Device procedure: full app restart, threshold 256, only
`ENABLE_EXECUTOR_INDEX_REBIND_TEST` and fresh `ARM_CAPTURE` in
`gpu_capture/pacman_world/`. Observe the same scene after the 60-present log
window too; the test persists until app exit. Send ZIP/log and visual result.

If this narrowly targeted intervention removes the triangle and traces exercise
the original nonindexed-quad/next-indexed sequence, combine that evidence with
the failing baseline host regression before promoting the structural repair.
Otherwise retain the demonstrated generic cache defect separately and continue
the graphics-state investigation. Do not call it the PAC-MAN cause solely from
source inspection or compilation.

No merge, broad permanent invalidation, extra submission, new barrier or unrelated
cache repair is authorized by this diagnostic conclusion.

## Validation and limits

The cache fixture compiles the complete production dirty-tracking header,
`IndexBufferState` declaration and methods and the new one-state invalidator.
It reproduces stale synthetic quad and inline bindings for both megabuffer and
direct backing. The explicit baseline `2ca42b93` fails four restoration cases;
the invalidator restores the guest binding and type without purging mapped-view
caches. Controls exercise unchanged guest draws, changed megabuffer allocation,
actual register dirtiness and existing cache purge behavior.

The command observer compiles the actual `HasIndexBufferBinding()` body against
command-header identities. This validates whether the prepared linked list
contains an index bind; it does not read the driver's current binding. Real
synthetic draw call sites are reviewed, not replaced by source-text assertions.
The host fixture does not execute the complete Android draw functions or GPU
vertices. The two override call sites and the mode gate require the APK/device
test as well as source review.

The existing conditional-rendering facade gained no-op declarations for the two
new trace methods while continuing to execute the unchanged full Draw body.
No production instrumentation is stripped for that suite.

## Resume audit

At resumption, both GitHub and the local branch still pointed to `2ca42b93`.
The mode-18 code, tests and this report were preserved as uncommitted changes;
the reflog and unreachable-object check contained no later local commit.
The existing regression was resumed rather than recreated.

No direct Vulkan test was found among the saved files. This environment has a
Vulkan loader but no ICD/device implementation, Vulkan headers or shader tools;
no candidate packages are available in its current package metadata. A direct
GPU run therefore cannot be claimed here. This is distinct from the existing
CPU host regression and the pending Moto G100 mode-18 test.

The remaining causal evidence is precise: the phone trace must show original
nonindexed quad/inline overrides, subsequent guest index rebinds in the same
submission, and the on-screen result under mode 18 **without modes 16/17**.
Until that evidence exists, the index invalidation remains an opt-in probe.

## Final local checks before publication

- Index-cache regression: **21 checks passed** with UBSan, including mapped-view
  cache preservation. The pre-interruption explicit baseline comparison had
  four expected restoration failures; that reproduction was retained.
- Executor diagnostics: **16 cases passed**, retaining modes 16 and 17.
- Conditional rendering: **20 cases passed** with ASan/UBSan.
- Submission/cycle safety: **26 assertions passed** with UBSan.
- `git diff --check`: passed. The pre-interruption independent review of the
  same production probe found no correctness/safety blocker; the remaining
  test-text checks were removed in favor of behavioral cache assertions.

The probe adds only the required index binding on the next indexed preparation
after a synthetic override; it does not flush descriptor/pipeline state or purge
mapped views. No FPS impact has been measured for mode 18. Driver pixels and
cross-game compatibility remain device-validation work. The existing workflow
requests ARM64 DevRelease when these commits synchronize the Draft PR; no
Actions completion or APK generation claim is made without observing it.
