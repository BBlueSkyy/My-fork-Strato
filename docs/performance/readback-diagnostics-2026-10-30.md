# Readback diagnostic plan and interpretation

Base: master 3900c30fa7b982f6f5369ccd8f7b6e51d23e6987. Separate diagnostic branch;
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
the emulation log. Record APK commit, Turnip version and approximate FPS. A second
run with only Fast GPU Readback disabled can be requested if the first log shows
its paths matter; no need to generate another APK. Do not remove the alias guard
or lower thresholds based solely on these counts.
