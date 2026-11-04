# Regional layout expansion implementation plan

The execution specification is the user's 2026-11-03 request for Draft #400.
Starting HEAD: `1475633e58f9480411d0b1a3cde841f0b15e6138`.
Remote master: `ab2011a42f629a6c458ade02fb4104a6d249cb69`;
merge base: `9935f13349c00d732ae599f4f90866b5b79629f5`.
Fresh checkout; no previous local changes. Preserve the existing branch and
all tests. Do not touch #421, merge master, change shaders or workflows.

## Design and constraints

Keep the dense-row path. For layouts rejected by that proof, use bounded
whole-GOB physical spans with explicit x/y/z, width, mip and layer coordinates.
Only bytes covered by an invertible image footprint acquire image authority;
padding remains guest-owned. Reject partial GOBs, invalid mappings, interleaved
Z and converted formats. No per-texel footprint bitmap or draw-time manager scan.
Planar 3D slices can share the regional protocol without broadening exact-alias
eligibility. Image copies and partial uploads must use each binding's own
coordinates and the correct packed-buffer row stride.

Alternatives: accepting padded rows wholesale would assign false authority to
unrepresented bytes; per-byte maps would impose excessive metadata. Both are
rejected. Existing draining remains the fallback for unproved conversions.

## Tasks

- [x] Audit registration, drain, CPU publication, buffer synchronization,
  sparse translation, remapping and executor publication. Record a coverage
  matrix, including existing mips/layers/fragmentation and remaining blockers.
- [x] Establish the unchanged Vulkan authority baseline. Add tests that fail
  for padded GOBs and differently tiled overlapping images, with independent
  physical address/data oracles, FreeGuest OFF/ON and failure/retry controls.
- [x] Add the minimal bounded footprint and transfer generalization. Preserve
  the dense common path, masked publication, resident-version commit and retry.
- [x] Add real Vulkan planar-3D/mip/slice cases. Enable only tested dimensional
  combinations; reject ambiguous geometry and unsupported copy alignments.
- [x] Review new synchronization/transaction boundaries. Do not advertise
  sparse, remap generations, cross-resource authority or executor rollback
  without integrated real-data tests. Record concrete prerequisites instead.
- [x] Run authority, lock-order, complete backing, linear readback and essential
  sequencing regressions; ASan/UBSan and synchronization validation. Review the
  diff and publish only validated changes to #400; request existing DevRelease
  via PR synchronization, without waiting for Actions.

## Review focus

Padding interpreted as pixels; padding shared with a different image's pixels;
changed GOB partition retaining old versions; packed upload row strides;
3D z versus array-layer addressing; retries with CPU/new donor mutations;
failed copies claiming residency; unbounded region counts; missing sparse
provenance/generation; no mobile FPS claim without device measurement.

## Execution record

See `docs/region-layout-expansion.md` for the coverage audit, RED/GREEN evidence,
final test commands and remaining Stage B/C/D prerequisites. Native execution
with one independent code review. Review blockers (trap mutex inversion, BC row
alignment, sparse residence provenance, quadratic GOB setup) were corrected and
validated; broader sparse/remap/buffer-image/executor transactions remain gated.
No phone performance or Android build result is inferred from host tests.
