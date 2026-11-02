# PAC-MAN Re-PAC — Stage 11: callback-only visual success

## Starting point and confirmed result

**Follow-up:** the user confirmed that mode 17 also removes the triangle.
Its 122 single-callback refreshes added no nodes/pending nodes and preserved
submission, cycle and tag. The narrowed index-cache contract investigation is
recorded in [Stage 12](pacman-repac-stage12-index-state-contract.md).

Continue Draft #414 at `04915dd58586ff8bc17fc23c4821084157d59ff8`.
Preserve the branch, all diagnostics, threshold 256 and no merge.

The user supplied `capture_001.zip` and `emulation.log`, then confirmed
**the triangle disappeared completely** with mode 16. Screenshot
`1002861888.png` shows the beach scene without the reported large triangle
and an overlay of **29 FPS**. A single screenshot is not an average-FPS test.

Evidence identities: ZIP Library ID `libfile_d2e60ae0c580819197c99aacfb4ef384`,
log ID `libfile_f458383bc4588191a269ddfc5daa1b9c`.
SHA-256: ZIP `739cf82ffa7a2b636c66c62a780d09b4772214093119603a2f4c8c1213ffa3bd`;
log `f8fffe6c480877c68f1aec9b31b2168aa836c14a47b8f90fd959088d91917f2b`;
screenshot `750b4ebeef12296c080f89a0b5d9ba7dd6c57225639329812fe2b35e63c31be6`.

| Recorded metric | Value |
|---|---:|
| Mode | 16 |
| Threshold | 256 |
| Host presents | 60 |
| Post candidates | 244 (61 groups of four) |
| Callback refreshes | 122 (slots 3 and 4) |
| Forced submits | 0 |
| Logged natural threshold submissions | 61 |
| Events | 1337 |
| Dropped events | 0 |
| Explicit callback-reentrant submissions | 0 |
| Deferred natural submissions inside callback bundle | 0 |
| Texture tracking saturation | 0 |

All 122 before/after callback pairs retained the same submission number,
render-pass index, executor cycle and execution tag. Each added exactly one
command node and one pending post-render-pass node. Threshold remained 256,
and none of these selected boundaries crossed the natural threshold.

Four relevant textures were observed in 364 complete before/after inline-sync
pairs (728 events): depth/stencil input format 129, output format 70, HDR input
format 122 and half-size HDR output format 122. All observations were GpuDirty,
GENERAL, generation 1, with one mapping span and no alias group. Every sync
reported `after_no_inline_upload`. This excludes inline upload of those four
resources in the observed sync calls; it does not exclude other resources,
earlier uploads or untracked overlap objects.

## What this establishes

The existing callback bundle is sufficient to remove the triangle in this run
without diagnostic queue submissions, slot rotation or execution-tag advancement.
Those operations are therefore not necessary for this particular successful
intervention. It does not establish which callback effect is responsible or
that callback replay should become a permanent rendering policy. Added query
commands can also affect execution timing and render-pass work.

Four callbacks are registered in this channel:

1. Graphics interconnect: descriptor lifetime handoff and invalidation of active
   graphics state, CB selector, sampler/pool caches and quick constant-buffer bind.
2. Maxwell engine: flush deferred engine/CBUF work and `FlushQueries()`.
3. Compute interconnect: invalidate compute pipeline/CBUF/pool caches.
4. Channel: increment `channelSequenceNumber`.

`Queries::Counter::Compact()` ends the active query, then schedules a resolve
if segments are pending. `EndActive()` adds an end-query command, and
`ScheduleResolve()` adds a pending post-pass command. This accounts for the
observed +1 node/+1 pending pattern; the counts alone do not prove no other
callback work occurred. The resolve helper uses the compute bind point and
compute push constants. No missing graphics-state notification has been proven.
`FlushQueries()` additionally resets prepared conditional-rendering operands
and their cached mappings. Earlier renderpass splitting already invoked
`queries.Pause()`/query ending, so that operation alone is not the newly
effective distinction.

## Smallest next discriminator and implementation plan

Run **one new test**: mode 17, `ENABLE_EXECUTOR_POST_GRAPHICS_STATE_TEST`.
After the same slots 3 and 4, invoke **only the existing graphics interconnect
flush callback**. Omit engine/query flushing, compute invalidation and channel
sequence advancement. That callback itself queues no nodes. Do not substitute
`NotifyPipelineChange()`, because its callback also pauses queries and would
retain that extra effect.

- [x] Verify the new capture, emulator log, visual result and current HEAD.
- [x] First add a failing behavioral regression using the complete production
  `AddSubpass()` and `AddFlushCallback()` bodies with labeled registration.
- [x] Label only the real graphics-state callback at registration with
  `FlushCallbackKind::GraphicsState`; all other callbacks default to General.
  Preserve callback order and ordinary Submit behavior. Store a diagnostic
  index from registration, never assume a hardcoded position.
- [x] Add mutually exclusive mode 17, preserving modes 0–16. Execute the one
  callback using the existing exception/reentry and post-callback threshold
  logic. A missing registration must produce no refresh count and an explicit
  unavailable event. Record mode and selected callback count in boundary logs.
- [x] Run host safety checks and review changes before publication. Request the
  existing ARM64 DevRelease PR workflow, **without following Actions**, as the
  user instructed.

If mode 17 removes the triangle, graphics-state refresh alone is sufficient
without added query work or a channel-sequence change in that run. Then narrow
the invalid graphics cache/state entry and test its structural repair.
If it fails, mode 16 still proves only the full bundle's sufficiency; examine
the omitted callbacks and interactions rather than applying blanket invalidation.
In particular, mode 17 does not advance the channel sequence; the same-sequence
TIC shortcut can still reuse its cached view even after its pool is marked dirty.
A negative result would not rule out texture-descriptor freshness.

Device procedure: full process restart; threshold 256; only the new mode-17
marker plus fresh `ARM_CAPTURE` in `gpu_capture/pacman_world/`. Expected
`executor_experiment_mode=17`, nonzero `executor_callback_refreshes`, and
`executor_experiment_draws=0`. Log pairs should select one callback and retain
node/pending counts unless another explicitly reported event intervenes.
The mode remains active after 60 presents. Supply visual outcome, ZIP and log.

No structural source defect is yet proven. No production rendering fix, global
barrier, shader-specific workaround or permanent callback refresh is published.

## Local validation

`tests/gpu_executor_diagnostics/run.py`: 16 cases passed with UBSan and compiler
warnings treated as errors. The same fixture compiles the complete production
`AddFlushCallback()` and `AddSubpass()` bodies unchanged. It verifies selection
by a label at shuffled callback index 2, excludes node-publishing general
callbacks in mode 17, retains mode 16's full bundle, and handles missing graphics
registration without incrementing the refresh counter. Natural-threshold and
next-subpass protections, bounded capture and post-capture persistence remain
covered. The missing registration tracking and mode-17 intervention initially
failed against `04915dd5`, before implementation.

Submission/cycle safety: 26 assertions passed with UBSan; conditional rendering:
20 cases passed with ASan/UBSan; queries: 15 cases passed with ASan/UBSan.
These checks use the existing host interfaces and do not establish Qualcomm
rendered-pixel correctness. The existing PR workflow runs the diagnostic fixture
and builds ARM64 DevRelease on branch synchronization. Its result is not being
followed, as requested. Mode 17's actual device outcome remains unknown.

Independent review found no correctness blocker. It confirmed registration-based
selection, unchanged ordinary callback order, the exclusive marker mapping,
optional index/range guards and bounded draw signatures. Full Android/Vulkan
translation units and the real graphics callback are not executed by the host
facade. Branch publication requests the existing PR workflow; build success is
not inferred from host tests and will not be monitored in this task.
