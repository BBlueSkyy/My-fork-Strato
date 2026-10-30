# Readback diagnostic plan and interpretation

Base: master b8593d8b2c2e9742a4f1f9edfaa935f653cd3663. Separate diagnostic branch;
no optimizations from Draft #401, no changes to #400, no merge.

The device test of #401 showed no perceptible Zelda improvement. The user reports
that Fast GPU Readback and Fast Readback Writes formerly helped but no longer do.
PR #341 added the no-alias condition to the texture shortcut; that condition is
absent from both good 28bcb957 and slow e7290a9b/9a6b48be, so it cannot establish
the initial #294 regression. The existing system trace shows fence waits but lacks
internal Strato track events and has import errors; no cause is proved.

## Design and implementation plan

Use lock-free relaxed cumulative counters at existing staged trap callbacks,
without changing their return decisions, thresholds, copies, fences or alias rules.
Alternatives were verbose per-trap logs (too intrusive) and another generic system
trace (cannot explain these internal decisions). The user approved spaced log
summaries. No new settings or workflow; diagnostics are always active in this
branch only. This is measurement, not a proposed performance fix.

1. Test mutually exclusive eligibility reasons, concurrent accumulation, the
   distinction between eligibility and actual execution, and a five-second gate.
2. Add one header, observe buffer/texture trap decisions and preparatory fence waits.
3. Emit two INFO lines from presentation after the existing frame callback and
   vsync signal, on the first presented frame and at most once per five seconds.
4. Review the source diff, run essential host checks, publish one Draft PR using
   the existing ARM64 DevRelease workflow. Do not rerun or merge.

## Log schema

Search the normal emulation log for `ReadbackDiag`. Each interval prints a buffer
line and a texture line. Values are cumulative from process start, not per-frame;
subtract consecutive samples of the same kind. Snapshots may straddle concurrent
increments and must not be treated as transactionally consistent.

- `fast`, `writes`: current options at report time; use unchanged settings per run.
- `read_traps`, `write_traps`: callback attempts including retries, not unique accesses.
- `not_dirty`: early clean/CPU-dirty (or stale texture) paths.
- `retries`: failed lock acquisition or still-pending cycle in read/write callbacks.
- `read_sync`, `write_sync`: completed normal SynchronizeGuest calls from these traps.
- `disabled`, `below_threshold`, `aliases`, `freed`: mutually exclusive primary
  reasons among write callbacks that reached the shortcut check. Priority is
  disabled, below threshold, aliases, freed. Aliases counts only otherwise eligible
  textures; it is not a total of all accesses to aliased textures. More than one
  underlying condition can apply even though only the primary reason is counted.
- `fast_eligible`: diagnostic snapshot satisfies the shortcut conditions.
- `fast_taken`: original shortcut branch actually entered. Concurrent state changes
  can cause a difference from eligibility; no diagnostic result controls execution.
- `fast_writes`: actual buffer shortcut left CpuDirty (Fast Readback Writes path).
  Textures have no corresponding path and always report zero here.
- `prep_waits`, `prep_wait_ns`: count and wall time of Wait calls in staged trap
  preparation only. Includes scheduling delays; not GPU timestamps or all Vulkan
  waits, and may overlap between threads. It cannot be summed as lost frame time.

No logs run inside trap/resource locks. Counters and timing add overhead; this
APK is diagnostic, not a benchmark baseline. No summaries appear while no frames
are presented. INFO logging must be enabled. Direct-import buffers bypass staged
traps, and the counters do not characterize every direct-import access.

## Validation and device collection

The header test passed locally with GCC, ASan and UBSan (LeakSanitizer disabled due
host process inspection restrictions). A full Android build and actual device
behavior remain CI/device validation. No FPS gain or compatibility claim.

On Moto G100: keep the same Turnip, save, mods, clock option and other settings.
Use INFO logging. Start Zelda with both readback options enabled, wait for shader
loading, play the same slow scene for 30 seconds, then exit normally and export
the emulation log. Record APK commit, Turnip version and approximate FPS. The shortcut was confirmed active on Moto G100; do not repeat its on/off
comparison without a new hypothesis. Do not remove the alias guard
or lower thresholds based solely on these counts.

## Frame-phase diagnostics (follow-up)

The latest device log confirmed 3,625 buffer fast paths and 3,624 fast-write
paths at 80.4 seconds, yet no perceptible gain. It also reported 1,417
preparatory waits totaling 13.92 seconds across potentially parallel threads.
This cumulative time does not equal main-thread stalling.

The new `FrameDiag` line prints **five-second interval deltas**, not cumulative
totals, after the first presentation establishes a baseline. All times are
CPU-side wall durations in nanoseconds; none is a Vulkan GPU timestamp.
`window_ns` and `frames` measure presented output throughput.
`present_cpu_ns` includes blocking presentation work and API calls.
`guest_fence_ns`, `swapchain_fence_ns`, and `acquire_ns` separate
presentation-side blocking stages. `texture_sync_ns` measures
SynchronizeHost, `display_copy_ns` CPU display-copy preparation and
`queue_present_ns` queueMutex acquisition plus presentKHR.
`record_slots` and `record_work_ns` describe recorded command slots
and time executing their nodes. `submit_ns` is the CPU wall duration of
SubmitCommandBuffer (including its internal synchronization), not execution on
the GPU. `acquire_slots` and `acquire_slot_wait_ns` measure command-record
slot backpressure; `completion_waits` and `completion_wait_ns`
measure the execution-waiter's fence completion wall durations.

Values across different threads may overlap, and must not be summed into one
frame budget. The diagnostic branch still does not measure guest CPU emulation,
all Vulkan work, or GPU device execution. It cannot by itself prove the
historical #294 regression. No readback thresholds, GPU ordering, copy
semantics, alias protections or workflow settings change. The diagnostic APK
has measurement overhead and is not a performance benchmark baseline.

## Device log interpretation (October 30, 2026): Zelda 1.0.0, Turnip 24.0.99

The 166.7-second diagnostic log confirms both readback settings enabled and
11,403 buffer fast paths, with 752 normal write syncs. The 120 texture write
trap attempts all found texture state not GPU-dirty, so this log does not
demonstrate texture alias blocking.

In approximately 50–130 seconds of continuous low-output gameplay,
FrameDiag recorded 16.68 presented frames/s, 3.05 recorded command slots
per presented frame, 26.41 ms of Host1x presentation-fence wait per frame,
1.19 ms recording CPU time per slot and 0.11 ms CPU submission time per
slot. This is a *Host1x host syncpoint* wait, not a direct Vulkan fence
wait. The execution waiter spent about 11.27 ms waiting per cycle, but
this includes scheduling and overlapping execution; it is not a GPU timer.
At about 140 seconds throughput fell to 8.64 presented frames/s, with
63.61 ms of Host1x wait per presented frame. The previous fast/menu
segments recorded around one command slot per presented frame, while the
slow scene consistently recorded around three. That correlation alone does
not prove the cause of historical PR #294.

The next experimental question is whether guest command processing has
already incremented the corresponding guest syncpoint threshold at
presentation wait entry. We now count four **mutually exclusive frame
classifications** in each five-second FrameDiag interval:

- `sync_no_fence`: no active Host1x fence supplied;
- `sync_guest_pending`: at least one supplied fence threshold is still
  ahead of guest syncpoint decode/command processing;
- `sync_host_pending`: all guest thresholds have been reached, but at
  least one host threshold is still pending, indicating completion and/or
  deferred host callback remains outstanding;
- `sync_host_ready`: all active guest and host thresholds are already
  reached at wait entry.

Each category has an associated `*_ns` wall duration. The classifications
are a single observation just before Wait(), not a state history; concurrent
updates may race with this observation. Multi-fence frames use worst
pending classification. This does not change guest synchronization,
command submission, GPU execution, presentation order, or fence waiting.
One counter update per classified frame plus two lock-free syncpoint reads
per active fence adds some diagnostic overhead. Compare only equivalent
settings and scenes; do not claim a FPS increase from these probes.
