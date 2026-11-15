# Native HDR regional capability

Run `python3 tests/gpu_hdr_region_sequence/run.py` with the native backing harness
dependencies documented in `tests/gpu_backing_promotion/README.md`. This reuses
the actual extracted production region, publication and image-copy bodies with
native Vulkan images, views, commands and fences. Scheduler, memory allocator and
trap dispatch interfaces remain host facades; this is not an Android emulator run.
The production mixed-authority gate remains false.

`--require-actual-coherence` deliberately exits nonzero: the exact physical-span
case is not admitted and retained readers remain stale. Default mode asserts that
counterexample and the conservative refusals, then runs a separate aligned HDR
positive control. A green default test does **not** repair that negative gate.

The 13 allocation intervals/sizes are reconstructed from PR453 at `88cc2d0d`.
Only the main HDR and valid donor geometries are confirmed. Secondary TIC formats
and some dimensions are missing: the test selects explicit synthetic HDR shapes
that reproduce their allocation footprints, not the unknown native layouts. The
five-mip 320x180 shape is also a hypothesis. Address arithmetic is relocated into
test storage; no logged address enters production.

The positive control deliberately uses different, full-GOB shapes. Six images
retain their backings and views through successive alternating GPU writes, a
mip0-only write, exterior preservation, publication/readback, CPU trap callbacks,
repair fault/retry and destruction/recreation. Both creation orders run. Raw packed
float patterns exercise finite, subnormal, Inf and NaN encodings. The independent
physical-byte oracle compares all logical pixel bits and full guest guard bytes.

The native pending-write case records a buffer-to-image copy, attempts the actual
overlap bridge before submission, verifies its typed submission request leaves
guest bytes untouched, then submits and verifies future regional readers. Other
pending/backing replacement and descriptor regressions remain in the full native
backing suite. This fixture does not implement HDR backing consolidation.

Use `SANITIZE=address,undefined` for ASan/UBSan. Optional compiler definition
`HDR_REGION_ALLOCATION_PROBE` counts thread-local host `operator new` calls during
600 already-coherent preflights; native image/staging allocations, submissions and
invalidation epochs are also checked. No Android FPS or frametime claim follows.

See `docs/region-stage-c-hdr-session-c-20261115.md` for evidence, limitations and
the decision to preserve current renderer behavior.
