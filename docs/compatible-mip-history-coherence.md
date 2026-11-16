# Compatible whole-mip history coherence

This change closes the native Vulkan coherence gate preserved at `f3e44e53`.
It does not establish that the visual MK8 bloom defect is resolved on Android.
The implementation contains no game, shader, brightness, LOD or blending policy.

## Reproduced condition

A smaller native image is created first. A second image covers a disjoint whole
mip in the extension of a later, larger request. PR451 correctly rejects its
prefix expansion because copying only the old prefix and initializing the rest
from guest memory would discard the second image's pending GPU writer.
The fallback creates an independent parent. Its later mip0 writes do not update
the retained smaller sampled view; exact-span alias groups do not join unequal
spans. Fixing only TIC invalidation still loses the independently written tail.

For the recorded compatible HDR allocation:

| Representation | Physical bytes relative to the shared base | Native mip |
| --- | --- | --- |
| 640×360, one mip | `[0, 983040)` | Parent mip0 |
| 640×360, two mips | `[0, 1310720)` | Full parent |
| 320×180 RT, when mapped to the suffix | `[983040, 1310720)` | Parent mip1 |

BH16, BD1, spacing0 and widthGobs40 establish physical compatibility for that
pair. The five-mip 320×180 bloom destination is another allocation and is not
this suffix RT. PR445 documents a changing parent and a byte-identical retained
bloom source; PR450 provides the compatible layout and child-first creation.
PR452 excludes mip1 feedback in its recorded reduction draws and observes an
unexpanded parent. Its first independent RT mapping was not logged, so identifying
that exact device object as the blocker remains an inference.

The historical managers before/after PR341 both have the containment/cache gap.
PR341 makes the nonzero-mip RT route reachable; this changes dependencies, but
neither these tests nor the existing captures prove exclusive visual attribution
to that PR. No old broad probes or route-splitting experiment are shipped here.

## Transaction

`TextureManager::GetMipDonors` accepts only an unambiguous set of native 2D,
single-layer, contiguous, whole-mip histories outside the existing prefix.
`Texture::MipDonorLevel` validates address/offset, full physical size, dimensions,
GOB geometry, effective block height/depth, width in GOBs, native format, format
list, samples and tiling. Conflicting mips, aliases, freed mappings, externally
published extents and foreign unsubmitted owners are rejected. Existing fallback
behavior remains available for unsupported histories.

After the existing executor submit/retry boundary, `PromoteGuestPrefix` locks and
revalidates all histories. The prefix identity is retained. Promotion copies its
old native levels and each donor's GPU image into only that donor's destination
mips. Only uncovered new levels are initialized from guest memory. There is no
new readback and no recurring reconciliation copy after consolidation.

Donor state locks remain held from upload snapshot through publication: CPU
WriteTrap can otherwise change Clean to CpuDirty without the main Texture lock.
Promotion-only transactional uploads prepare fence ownership before submission
and retain CpuDirty/layout state after allocation or recording failures.

All live donor logical TextureViews are rebound under the global GPU channel lock
and both resource locks; baseMipLevel is offset, levelCount/layers remain intact.
Old native images, views and framebuffers remain attached to their actual fence
owners. Retired donor traps cannot restore obsolete guest bytes. A backing/view
epoch forces one full graphics descriptor refresh after identity changes; warmed
frames reuse descriptors. The ordinary LockableSharedPtr lock path is unchanged.

## Verification

Tests run with clang19, UBSan, Mesa lavapipe and Vulkan synchronization validation.
Actual Vulkan images, render-pass writes, copies, queue submissions, fences and
readbacks are used. Production manager, promotion/sync functions, TIC-hit branch
and descriptor decision branch are extracted into the existing host facades;
this does not execute the complete Android renderer, guest shaders or real engine.

The ordinary `tests/gpu_backing_promotion/run.py` now includes the former opt-in
red gate plus these regressions, in both image-format-list modes:

- Both creation orders, successive mip0 writes and retained sampled TIC views.
- A native write recorded against the old suffix view before parent resolution;
  executor submission occurs before its copy, and its pixels reach parent mip1.
- HDR two-mip and alternate RGBA dimensions/three-mip chains, multiple disjoint
  GPU donors with an uncovered CPU mip, reverse writes and retained-view lifetime.
- CPU write contention between donor snapshot and canonical trap publication.
- Initially CPU-owned donor upload allocation/recording/attachment failures;
  retry preserves bytes. Promotion allocation/record/attachment/trap rollback.
- External/foreign/freed/incompatible histories rejected. No additional images,
  backing generations or descriptor rebuilds over warmed frames.
- Zero Vulkan validation errors.

Negative controls were observed, not inferred: checkpoint code fails with stale
TIC content; invalidation alone loses the pending tail; removing the descriptor
epoch guard fails the native-handle refresh assertion; donor state and upload
failure tests fail before their corresponding fixes.

Related suites passed: mapping/multirange, trap-prefix rollback, sampled-layout
and aliases, framebuffer cache, draw textures, submissions, null storage images,
conditional rendering and queries. Submission tests also verify retired donors
skip obsolete uploads. A preexisting missing InvalidateIndexBuffer API in the
conditional test facade was reproduced on master, then the facade was updated.
ASan suites use detect_leaks=0 because LeakSanitizer cannot run under this host's
ptrace environment; no absence-of-leaks claim is made. Android compilation and
artifact verification belong to the PR workflow.

## Acceptance and risks

MK8 must still be checked on Moto G100 in the same Stadium scene, same Turnip
version, Free Guest Texture Memory OFF, plus scene changes and consecutive races.
Retest SMO, BOTW and RE5 for rendering, loading, memory pressure and stability.
Host allocation counters do not establish Android FPS, frametime or memory usage.
The registry adds weak view metadata; a rare epoch change refreshes descriptors.
Foreign, ambiguous, or incompatible histories remain outside this consolidation
and can still require broader coherence work. No merge is authorized.
