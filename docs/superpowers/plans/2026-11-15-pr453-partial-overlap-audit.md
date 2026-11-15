# PR453 partial-overlap audit implementation plan

**Goal:** Explain the actual Android consolidation block and test whether removing
it can preserve all relevant content, without publishing a speculative renderer change.

**Architecture:** Keep current production policy unchanged during evidence review.
Use the existing extracted-production/native-Vulkan harness for recorded interval
geometry and controlled ownership variants. Only implement runtime changes if a
safe exception or reconciliation mechanism is demonstrated; otherwise publish the
necessary-guard conclusion, counterexamples and precise missing device evidence.

**Checkpoint:** PR453 head 619454720342c58578a264295c6994a3d47418d4;
master f98c9bb6b80a2bc145e07058176b82d9917ae213 is already its parent.
Source log: 318 lines / 33452 bytes, SHA256
0005a81794db80835942818cb0b89827bdeb909199ec0ce9436d382361f94337.

## Constraints and review focus

- Update existing Draft453 only; preserve all checkpoints and other PR branches.
- No shaders, LOD, brightness, new capture system or broad instrumentation.
- pending=false/serial0/finished readback never prove a history can be discarded.
- Native image-copy boundaries are mips, whereas logged partial overlap is inside
  one mip and extends outside the requested parent.
- Preserve retained readers, future writes, guest padding and out-of-parent bytes.
- Existing exact-span AliasGroup cannot silently become an interval authority map.
- Android APK only after a proven fix and all tests; verify workflow/artifact then.

## Tasks

- [x] Read entire attachment before modifications; verify current remote/head.
- [x] Correlate all 40 emitted events with once-only probe and real callsites.
- [x] Save ordered event inventory/interval table and compare recorded vs unknown states.
- [x] Add native Vulkan partial-history fixture: recorded prefix, valid tail,
  shifted 5/1-mip histories; preserve dimensions/formats as explicit fixture
  assumptions where device log omits them. Verify valid-only control, block for
  partial histories, pending writes, retained sampled views, CPU/GPU ownership,
  post-readback future writes, failure rollback and all mip data.
- [x] Run a host-only unsafe-guard negative control and show a content failure,
  not just a validation-layer or promotion-status assertion.
- [x] Run existing TexMan/alias/trap/framebuffer/descriptor/submission/lifetime suites.
- [x] Review evidence/tests, commit/checkpoint and document on existing PR453.
- [x] Runtime patch/build only if a correct mechanism is proven; otherwise keep
  conservative block and explicitly identify missing authority information.

Ruling: no renderer patch is supported by the log or counterexamples. The guard
is necessary for the whole-mip transaction; retained partial readers require
physical-interval coherence even after completed readback or serial-zero upload.
This task therefore publishes tests/evidence only, with [skip ci] to obey the
user's conditional APK requirement. No new diagnostic/build or merge.
Ruling: secondary dimensions/formats absent from device records remain explicit
fixture assumptions. Exact spans are reproduced; Android TIC identity is not.

Final review: source-log counts/intervals independently verified. Fixture offset
corrected after a failing address-difference assertion, then full native suite
and ASan+UBSan target rerun green; promoted tail compared to retained native
donor backing. No preservation claim for guest GOB padding. No deferred minors.
Pending publication: only test/docs updates to existing PR453, [skip ci].
