# PAC-MAN Re-PAC — synthetic index binding invalidation

## Cause and device evidence

The guest index-buffer cache remained valid after host-generated indices
replaced the Vulkan binding. A later indexed draw could skip its bind because
the guest registers and cached megabuffer binding were unchanged, while Vulkan
still held the generated indices. This is a binding-state ownership error,
not evidence of a missing texture barrier.

The host regression reproduces guest binding → synthetic override → unchanged
guest draw retaining the wrong binding. The mode-18 phone test changes only
index invalidation, independently of postprocess slots or graphics callbacks.
The user confirmed that it removes the PAC-MAN triangle; the screenshot shows
**30 FPS** (a single observation, not a benchmark).

Captured on Moto G100 / Adreno 650 / Qualcomm proprietary 0.777.0:

The instructed setting was threshold 256; this mode's text trace does not
serialize the setting itself. The correction does not change that setting.

| Metric | Recorded value |
|---|---:|
| Mode | 18 |
| Presents | 60 |
| Dropped events | 0 |
| Forced submits | 0 |
| Broad callback refreshes | 0 |
| Synthetic overrides / narrow index invalidations | 1,652 |
| Original guest indexed flag | false for all 1,652 |
| Original guest count / first | 4 / 0 for all 1,652 |
| Subsequent prepared guest index binds | 294 |
| Binds within the same submission as the preceding override | 236 |
| Binds after an ordinary submission boundary | 58 |

All 294 observed guest binds report `emitted=true`. Slots 3 and 4's shaders
appear among the original nonindexed quads (59 occurrences each), establishing
that their old `indexed=true,count=6` metadata described converted operations.
No inline-index draw was observed in this phone capture. The analogous inline
clobber is covered at cache level by host regression; the actual
`DrawWithInlineIndex()` call site is source-reviewed, not executed by that
fixture or claimed as PAC-MAN's active path.

ZIP ID: `libfile_19f2c895dc488191bc1e6796baa51afb`;
SHA-256 `64d56dc357fd861c5a209095cfa7c6539ec5959f683bac08d8d02fbcae63e1c6`.
Log ID: `libfile_002f6f0c849c8191bb033395829d31f1`;
SHA-256 `8457b4da9e7f2007cd3d7bfc697f714ec2be90ca51d07ba1d8a2006cd4d0dbc4`.
Visual outcome is the user's confirmation plus screenshot `1002861929.png`;
the text capture alone contains no framebuffer pixels.

## Structural correction

Immediately after each synthetic `SetIndexBuffer()` in
`Maxwell3D::Draw()` (nonindexed quad conversion) and
`Maxwell3D::DrawWithInlineIndex()`, call
`ActiveState::InvalidateIndexBuffer()`. It sets `indexBuffer.MarkDirty(false)`:
retain mapped-buffer caches, require the next indexed preparation to rebind.
Invalidation is independent of all diagnostic markers and game properties.

The existing mode-18 trace remains available as observation of the corrected
path. Its trace helper must no longer perform a second invalidation. Modes
16/17 and every earlier diagnostic remain available. Final phone acceptance
should use **no ARM_CAPTURE or ENABLE_EXECUTOR_* marker**, threshold **256**.

No new submits, waits, global barriers, descriptor/pipeline refreshes, texture
ownership changes or rewritten executor/TextureManager are required.

## Validation, performance and scope

The existing cache fixture exercises actual production index-cache methods and
dirty tracking for generated quad and inline overrides, direct and megabuffer
backings, index type restoration and mapped-cache preservation. Integration
coverage must additionally ensure the real nonindexed-quad Draw path invalidates
the cache while capture is unarmed; ordinary draws must retain their cache.

Invalidation is a sticky dirty bit; consecutive synthetic draws do not force
guest index binds until indexed guest state is prepared again. Mode 18 observed
236 relevant binds within existing batches over 60 host presents, avoiding the
previous blanket state refresh or additional queue-submission interventions.
Expected overhead is narrow binding preparation; average FPS/frame pacing and
final unarmed APK behavior remain device checks.

PAC-MAN's causal diagnosis is supported by the narrow intervention, real
nonindexed-quad sequence and host reproduction. This is not a claim that every
Unity title or visually similar issue has the same cause. Independently test
Marsupilami: Hoobadventure and Prince of Persia: The Lost Crown, plus the other
usual compatibility titles, before asserting those regressions are resolved.

No merge. Keep Draft #414. Request ARM64 DevRelease through the existing
workflow without monitoring Actions, per user instruction.

## Completed host validation

- Cache/binding regression: 21 checks passed with UBSan.
- Full-Draw/conditional integration: 22 cases passed with ASan/UBSan. The new
  unarmed quad→guest-indexed case failed before the unconditional call-site
  change and passed afterward; ordinary and already-indexed draws retain a
  clean cache. The actual Draw, trace helper, invalidator and dirty wrapper
  execute; PrepareDraw, retained guest view and recording remain facades.
- Executor diagnostics: 16 cases passed, preserving all prior modes.
- Submission/cycle safety: 26 assertions passed with UBSan.
- Independent source review found no correctness or safety blocker;
  `git diff --check` passed.

The existing PR workflow now runs the full-Draw integration suite alongside
the diagnostic and index-cache suites before building the ARM64 DevRelease APK.
APK completion is not monitored and no successful build is assumed.
