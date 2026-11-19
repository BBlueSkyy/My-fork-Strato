# Oceanhorn 2 — GPU occlusion-query report A/B (diagnostic only)

The user's Oceanhorn 2 renders distant objects only when close in Strato;
Eden does not exhibit it. The first PR #469 A/B forced conditional
rendering, which did **not** help. Its control log had >151,000
sample-counter reports, but no `CheckRenderCondition` calls.

## Revised experiment

- CONTROL `-PoceanVisibilityDiagMode=1`: preserves real query-report
  bytes and records a sparse GPU snapshot of the real counter and the
  destination GPU buffer, at report 1 and every 8,192nd report.
- FORCE POSITIVE `-PoceanVisibilityDiagMode=2`: same snapshots but writes
  1 (32-bit low word) and, for 64-bit reports, 0 (high word) to guest report
  memory, **without** changing actual sample counters, accumulator values,
  or the timestamp stored at destination +8. No conditional-draw override.

Both variants keep PR #317 and all current master fixes.

### Build artifacts (ARM64 DevRelease, one at a time)

- `strato-pr469-arm64-release.apk` — CONTROL.
- `strato-pr469-arm64-force-query-positive.apk` — FORCED.

Log records:
`[OCEAN-QREPORT]` reports/guest addresses,
`[OCEAN-QRESULT]` report serial, actual `gpu_samples` and `guest_result`,
count of segments, reset flag, 32/64-bit width, forced flag.
`[OCEAN-COND]` logs conditional draw activity, if any.

This probe copies the real accumulator to a 16-byte mapped GPU buffer
before any override. It copies guest report output to the other half
after writing and after the existing report barrier, then adds
Transfer→HostRead synchronization. A cycle-completion callback waits
for the GPU fence before reading either snapshot. Probes are sparse
(about 19 per 151,552 reports); on non-DMI hosts each one can cause a
brief stall. No blocking CPU readback for unsampled reports.

### On-device A/B validation

1. CONTROL: same save, camera, driver and settings. Reproduce missing distant
   objects and export emulation.log and screenshot.
2. FORCED: at exactly the same camera position, verify whether objects
   reappear; export emulation.log and screenshot.
3. Check `[OCEAN-QRESULT]`: in FORCED guest_result must be 1; in CONTROL
   guest_result should match gpu_samples (modulo 32-bit output width).

If forcing positive restores geometry, investigate sample reporting and
guest consumption, including precision and timing, before attempting fixes.
If it does not restore geometry, prioritize LOD, streaming, shader geometry,
indirect draw submission, and depth state. The logs do NOT automatically
prove which engine path is wrong. Forcing positive may introduce side
effects in unrelated games; the experiment must **never be merged**.

Default mode 0 is unchanged; this PR is intentionally diagnostic only.
