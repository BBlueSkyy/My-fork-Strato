# PAC-MAN Re-PAC — Stage 10 submission contract investigation

## Scope and starting state

Continue Draft #414, branch `debug/pacman-repac-triangle-mini-renderdoc`,
starting at `461eb32081c0f677da0891db4389723879ebd892`. The working tree was
clean. All 66 existing commits and the Stage 1–9 instrumentation are retained.
The base was `fc8eb5196c198b281dce6bb3d9a9126b11ac5d4a`.

Default threshold remains 256. No merge, permanent extra submission, global
barrier, shader/format/dimension-specific production fix, or unrelated repair.

## Verified new device evidence

The duplicate uploads with `(1)` in their filenames were materialized and read.
Visual outcomes are the user's observations; the ZIPs contain metadata, not pixels.

| Pair | Mode | Presents | Candidates | Extra submits | Dropped | Visual result |
|---|---:|---:|---:|---:|---:|---|
| 1+3 | 13 | 60 | 244 | 122 | 0 | Residual triangle |
| 3+4 | 15 | 60 | 236 | 118 | 0 | Triangle absent |

Both modes persisted after logging. Logs identify Qualcomm 0.777.0 and
`executor_test=13/15`. Host presentation intervals are not guest frame IDs.
The 3+4 result establishes sufficiency for that run, not a direct dependency
between the two candidates or a particular missing Vulkan barrier.

## Code paths and remaining uncertainty

`AddSubpass()` queues the draw before testing the threshold or diagnostic
selection. `Submit()` then performs these distinct operations:

1. Persistent flush callbacks: Maxwell active-state/descriptor/pool invalidation,
   constant-buffer quick-bind reset, engine/query flushing, compute cache
   invalidation, and increment of the channel sequence number.
2. A new execution tag, deferred-action scheduling, and completion waiter setup.
3. `SubmitInternal()`: end render pass; drain pending post-pass nodes; wait for
   the recording slot; record the existing barriers and CPU-dirty texture uploads
   at the beginning of the command buffer; order predecessor submissions and
   chain cycles; synchronize buffers; rotate the recording slot.
4. Attachment unlocking/reset, render-pass usage reset, and allocator reset for
   the next slot.

The old traces expose texture object/cycle identities at candidates, but not
host mapping spans, dirty state at upload, descriptor invalidation effects, or
the state before/after callbacks. A texture's recorded cycle at `AddSubpass()`
normally describes earlier submitted work; it is not proof that the current
producer has been submitted. `ChainCycle()` lifetime retention alone is not a
Vulkan memory dependency; existing `WaitSubmit()` ordering must remain intact.

No specific violated contract is yet established. Code audit found potential
cache/layout and overlap paths worth checking, but none is proven active in
the corrupted execution. They are not included as speculative fixes.

## Minimal diagnostic plan

- [x] Verify the current branch/PR and read Stage 8/9 plus paired captures.
- [x] Run the existing submission, sampled-layout/alias and query host baselines.
- [x] Add a behavioral host regression for callback-only mode before implementing it.
- [x] Add mode 16, `ENABLE_EXECUTOR_POST_CALLBACKS_TEST`: invoke the **same flush
  callbacks only** after candidates 3 and 4, with the existing `gotoNext` guard.
  Keep normal threshold submissions. Do not rotate slots, change execution tags,
  unlock attachments, or run `ResetInternal()` as part of this intervention.
- [x] Log callback entry/exit node counts, render-pass index, cycle, execution
  tag and submission number. Callbacks may themselves publish deferred draws,
  CBUF writes or query work, so this is a callback-effects test, not a pure
  descriptor-only experiment. Log any reentrant/natural submission explicitly.
  A nested draw's threshold submission is deferred until the outer callback
  bundle exits, then the node count is reevaluated. Direct reentrant `Submit()`
  is logged rather than suppressed. The execution-tag comparison uses equality,
  since converting `ContextTag` to an integer would only expose its boolean value.
- [x] Observe the relevant sampled/output textures at candidates 3/4 and around
  their normal `SynchronizeHostInline()` calls: host mapping spans, backing
  generation, layout, dirty state, write/readback serials, cycles and usage.
  Lock dirty state briefly; never hold that lock across synchronization or logging.
  No readbacks, new waits, resource replacement or synchronization changes.
- [x] Preserve all existing modes; execute host checks and review the diff.
- [ ] Publish diagnosis separately from any future proven correction, and verify
  the existing PR Build produces the ARM64 DevRelease APK.

## Device acceptance and interpretation

One new test is sufficient for the next decision: threshold **256**, same driver
and scene, full app restart, only `ENABLE_EXECUTOR_POST_CALLBACKS_TEST` plus a
fresh `ARM_CAPTURE` under `gpu_capture/pacman_world/`. Expected mode **16**.
Observe long enough after capture completion; the mode persists until exit.
Send ZIP, log, screenshot and approximate FPS.

- If callback-only mode removes the triangle, callback effects are sufficient
  without extra queue submissions in that run. Next isolate the specific cache,
  descriptor, engine or query callback; this is not proof of a particular one.
- If the triangle persists, callbacks alone are insufficient. The next decision
  must use the observed texture uploads/cycles and investigate execution-tag
  cache invalidation, attachment synchronization, rotation/lifetime and scheduling.
  Failure does not exonerate all state caching, since execution tags are unchanged.
- If no selected callbacks run, mixed markers disable the mode, events are dropped,
  or callbacks induce unexpected reentrant submits, interpretation is inconclusive.

Mode 16 counts callback interventions separately from extra submissions. No
graphics fix is claimed by host tests or an APK build. PAC-MAN device acceptance
is still required; Marsupilami and Prince of Persia require independent checks.

Completion metadata uses `executor_callback_refreshes` for mode 16's selected
callback bundles and leaves `executor_experiment_draws=0` for that mode. Ordinary
threshold submissions are logged as `EXECUTOR_NATURAL_SUBMIT`, not counted as
forced interventions. Approximately 118 callback refreshes are expected if the
same candidate sequence repeats. A zero count is inconclusive.

`TEXTURE_SUBMIT_SYNC` reports `dirty=0/1/2` for Clean/CpuDirty/GpuDirty.
For guest optimal textures, `after_inline_upload` means the sync changed the
texture cycle from a distinct predecessor to this executor cycle. Only the
staged-upload branch does that. A preexisting self cycle is explicitly unknown;
dirty-state observations alone are not upload proof. Logging itself cannot
exclude timing perturbation.

Texture tracking is bounded to the first eight unique watched object addresses,
with at most four host mapping spans printed per texture. `TEXTURE_TRACK_LIMIT`
or a mapping count greater than four means lineage coverage is incomplete.
Addresses can be reused after object destruction; compare image, generation and
mapping spans as well. These spans identify guest memory's host mappings, not
physical GPU addresses. They can establish an overlap when shown, but partial
mapping lists cannot prove that no overlap exists.

## Verification ledger

Baseline: `tests/gpu_submission/run.py`, `tests/gpu_sampled_layout/run.py`, and
`tests/gpu_queries/run.py` passed. Submission tests use UBSan; sampled layout
and query fixtures use ASan/UBSan. These do not validate proprietary-driver pixels.

The new full-`AddSubpass()` behavioral fixture passed 12 cases with UBSan and
warnings treated as errors, including callback-published nodes/resources,
exception/reentry guards, post-callback threshold crossings and all old modes.
It first failed against the starting implementation at the missing callback-only
intervention, and the threshold regressions separately failed before the guard
and reevaluation were added. It models Vulkan/engine interfaces; it is not a
pixel correctness test. The existing PR Build now runs this fixture too.

Submission/cycle checks passed (26 assertions); sampled-layout and alias checks
passed; query checks passed (15 cases); multirange mapping/overlap/ownership
checks passed with ASan/UBSan. Leak scanning alone was disabled for multirange
because LeakSanitizer cannot enumerate threads in this container.

The conditional test initially failed to compile because its host facade did
not include the Stage 1–9 Draw diagnostics already present at `461eb320`.
The starting and current Draw bodies were compared to establish that baseline
failure. The facade now includes the real unarmed capture header and the missing
interfaces, without removing production instrumentation; all 20 conditional
cases passed with ASan/UBSan.

Independent review found no remaining correctness blocker after the
post-callback threshold/reentry tests and changes. Its bounded-identity and
explicit-reentrant-submit limitations are recorded above. No synchronization,
ownership, alias or packed-depth production repair was applied without device
evidence.

Android CI and on-device results remain pending until explicitly recorded below.
