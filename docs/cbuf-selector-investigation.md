# Constant-buffer selector rejection after B1a — evidence and capture

## Status

Diagnostic-only continuation of Draft #400 from
`de44f1fc6fce9c6f9f767f2323d8f7987c1b28fa`. Master inspected at `ab2011a4`.
All twenty existing commits, Stage A and B1a are preserved. No B1b/B2, kernel
allocation hook, buffer/image authority unification, shader or workflow change.
#421 is untouched.

The Moto G100 report suggests a B1a regression but does not prove its cause or
reproducibility. No behavioral correction is claimed or enabled here.

The supplied `emulation(20261103-205507).log` records SMO **v1.0.0**, no update,
Turnip **24.0.99 / Vulkan 1.3.280**, and at timestamp 149367904:
`Constant buffer selector is not mapped`, in `ConstantBuffers::Bind`, called
through `MacroInterpreter::Execute` and GPFIFO. It contains no selector IOVA,
requested length, mapped extent, cached view, generation, macro code or arguments.
It therefore cannot distinguish absent translation from stale preparation.
Free Guest and mod status cannot be established from that log.

Reported device comparisons (not rerun by these host tests): master apparently
works on the tested route; `1475633e` and `feb00c3d` show castle corruption without
this observed crash; `de44f1fc` crashes. Complete control runs and repeatability
remain outstanding. The early-level fog improvement predates A/B1a and is not
attributed to this change.

## Source audit

The **entire files** `constant_buffers.cpp`, `common/common.cpp` and
`macro_interpreter.cpp` are byte-identical at master, `1475633e`, `feb00c3d` and
`de44f1fc`, before this instrumentation. `3c1c73b1` is documentation only;
`de44f1fc` only makes the texture-header span reset explicit for Android Clang.

The actual bind path is:

1. The interpreter's `Send` calls `Maxwell3D::CallMethodFromMacro` → `HandleMethod`.
   Shadow RAM can track/replay the method before the bind-group callback.
2. The enabled bind-group callback invokes `ConstantBuffers::Bind` with the
   decoded stage/slot. `selectorState.UpdateGet(ctx)` either Flushes its dirty
   selector or calls Refresh. Flush asks for `max(minSize, selector.size)`;
   Bind's minimum is zero. Refresh updates only when the stored **view size**
   differs from the requested size; dirty tracking handles selector-register changes.
3. `CachedMappedBufferView::Update(..., splitMappingWarn=false)` uses the cached
   block or `LookupBlock` and preserves the existing first-block clamping of an
   upper-bound selector length. It clears the view on a nonresident first block.
   Neither removing clamping nor suppressing that rejection is justified.
4. Bind throws if the resulting view has no delegate, before replacing the bound
   constant-buffer slot. A zero-sized view is not by itself proof of that failure.

CBUF caches do **not** own B1a image translation receipts. A generation-zero
observation below means no currently tracked association, **not** an unmapped
physical page. Receipt invalidation by itself does not directly clear a CBUF.

An indirect mechanism still needs investigation: B1a may cause additional
operation retries/submissions; the existing Maxwell executor flush callback
purges constant-buffer selector caches. That could expose a changed/unmapped
selector earlier. This is a hypothesis, not a demonstrated crash explanation.
The unchanged sparse/null semantics of `LookupBlock` and existing selector dirty
tracking also need to be compared against the recorded failing state.

## Temporary bounded instrumentation

- Eight fixed scalar events per selector: actual Flush/Refresh updates and
  PurgeCaches; request/minimum, before/after cache bounds, numerical host base,
  view validity/size, mapping revisions and a purge count. Successful binds emit
  no logs; no allocation or resource lookup is added to a successful bind.
- At most **two** `CBUF_DIAG` failure reports per process: live selector/stage/
  slot, the bounded history, and at most eight block observations of the current
  IOVA range. A size-zero selector probes one address without reading its bytes.
- `MappingIdentity::Inspect` reads existing interval identity without capturing a
  receipt, minting a generation, reclaiming metadata or modifying authority.
  Each probe releases its locks before logging. Probe start/end revisions detect
  overlapping GMMU mutation; mixed-revision observations must not be treated as
  a consistent translation snapshot. A null block is reported as nonresident/
  sparse; it is never interpreted as zero-filled data. Zero extent or the eight-
  block cap leaves an explicitly incomplete probe.
- At most **two** `MME_DIAG` macro failure reports: entry/PC, exact failing Send
  method and input argument, register state, argument count/consumed offset,
  first sixteen arguments, first sixteen entry instructions and at most seventeen
  PC-neighbor instructions. The reported Send argument precedes Maxwell shadow
  replay; decoded Bind stage/slot and live selector reflect the effective state.
- All diagnostic output is best-effort. Logger failure preserves the original
  exception. Nothing retries, substitutes a buffer, reads guest payload bytes,
  submits commands, waits for GPU completion or alters mapping validity in
  response to the diagnostic. Cached numerical host addresses are metadata only,
  never dereferenced by the reporter.

This adds roughly 1.2 KiB of fixed selector history per Maxwell instance, a small
scalar cache observation on actual updates/purges, and three scalar macro failure
fields. There is no new per-draw GMMU scan, receipt allocation or successful-bind
logging. Device performance has not been measured. Remove this temporary capture
once the failure is explained and a focused regression replaces it.

## What to capture next on Moto G100

Use the diagnostic ARM64 DevRelease from the same Draft #400. Keep the same save,
route into Peach's Castle, driver, game version and settings as the failing run.
Record Free Guest ON/OFF and any mod/cheat status; keep those identical for each
baseline comparison. No special log level or manual capture toggle is needed.

Export the complete emulation log immediately after the **first** crash, including
`CBUF_DIAG`, `MME_DIAG` and the final stack. Avoid truncating their preceding
history. If the crash does not recur, record that outcome rather than declaring
it fixed; repeat cold launches of the same route to establish frequency. Test
Free Guest changes separately from the primary reproducibility comparison.

The next analysis should distinguish:

| Observation | Interpretation to verify |
|---|---|
| Stable revision, live selector starts at a null mapping | Trace guest mapping removal and macro/selector address; do not fabricate a buffer |
| Cached bounds/physical base differ from a stable current lookup | Determine dirty/purge ordering and whether an old view was retained |
| Revision changes during update/probe | Repeat with the bounded state; diagnostic reads are not an atomic snapshot |
| Selector size/minimum and view size differ, resident prefix exists | Check established clamping and the actual register dirty path before changing sizes |
| Failing Send method/input differ from decoded Bind | Check macro program/arguments and shadow replay, not just the throwing function |
| Extra purges appear before the failing Flush | Correlate with B1a submission/retry boundaries; counts alone do not establish causation |

## Host validation scope

`tests/gpu_cbuf_selector/run.py` compiles exact production selector Flush/Refresh/
Purge/Bind, cache Update/Purge, ManualDirtyState and complete macro interpreter
bodies. GMMU map/unmap/translation functions are production bodies. Block-table,
BufferManager storage, register dirty notification and logging transport are host
facades; this is not an Android SMO reproduction or a Vulkan CBUF execution.

The test checks independent CPU byte patterns for dense/clamped/partially
resident requests and P→Q reprepare, unchanged original bound bytes after failure,
sparse/unmapped rejection, increasing minimum sizes, eight-event history wrap,
no successful-bind logging, two-report caps, non-mutating identity inspection,
exact macro Send values and preservation of the original exception even if
logging throws. Before instrumentation, its diagnostic assertions fail on both
the production Bind and interpreter paths. ASan/UBSan remain enabled.

The preserved B1a identity, GMMU, authority/content, pending-reader, cache/RT/pool,
cancellation/retry, snapshot-limit and lock-order gates must pass before publishing.
The original A→B→A oracle remains 0/1024 with Free Guest OFF/ON. No game crash fix,
APK success, phone performance or corrected castle rendering is inferred from
those host tests.


Final host results (Clang 18, lavapipe/Khronos synchronization validation):

| Gate | Result |
|---|---|
| New `gpu_cbuf_selector` | PASS, ASan/UBSan; deliberate Bind/interpreter negative controls failed before diagnostic implementation |
| `gpu_mapping_identity`, `gpu_gmmu_identity`, `gpu_region_residency` | PASS; generations, concurrency, lifetime, snapshot/split ceilings and sparse/null proof preserved |
| `TEXMAN_AUTHORITY_PROBE=1` + ASan/UBSan | PASS; 0/1024 original A→B→A, FreeGuest OFF/ON × format-list OFF/ON; real P→Q→P, pending readers, RT/pool/cache, cancellation/retry and regional-content checks |
| Full `gpu_backing_promotion` and both `TEXMAN_LOCK_ORDER_PROBE=1` modes | PASS; zero Vulkan validation errors |
| `gpu_texture_sequence`, `gpu_submission`, `gpu_sampled_layout`, `gpu_storage_preflight`, `gpu_overlap_materialization`, `gpu_framebuffer_cache` | PASS |

ASan leak checking is disabled for this host's task-inspection restriction; address
and undefined-behavior checks remain enabled. The new fixture requires Clang,
matching Android's production macro annotations. APK compilation/device outcome
are pending; PR synchronization requests the existing ARM64 DevRelease build
without waiting for or monitoring Actions.
