# HDR prefix coherence: reproduced defect, unresolved device layout

Status: **reproducer and narrowly scoped layout diagnostic, not a graphics fix**.
Base: master 505bc7d1. Protected PRs #400/#445/#448/#449 are not changed.

## Evidence checked

The original latest archive contains 25 MK8_640_ALIAS entries. It identifies
the one-mip 983040-byte source and two-mip 1310720-byte writer at the same
physical start, independent backings and null alias groups. Its source
readbacks are reported identical while the writer receives later draws.
The four source files' SHA-256 was independently recalculated and matches
2b55909ce4aa4556df94e1c522565624f80b3b0d7c2d37c8cedb3686bb7c75d3.
The archive does not record declared/effective GOBs or width spacing of the
one-mip source. The 640x360 RT declarations show Y GOB 16.

640x360 at four bytes per pixel occupies 983040 physical bytes with **both**
BH8 and BH16: padded height is 384 in both cases. Their GOB ordering differs;
equal addresses/byte counts are not sufficient for Vulkan-image sharing.
The BH16 two-level layout occupies 983040 + 327680 = 1310720 bytes.

## Code conditions established

- MatchMappingSubrange returns offset=0/full=false for a contained shorter
  prefix. That is correct; changing full to true would violate exact-alias
  registration's identical-span assumptions.
- Existing parent first, compatible child later: blockMipCandidate can match
  dimensions, BH/BD, size, mapping offset and view bounds and reuse mip0.
- Existing child first, larger parent later: FindOrCreate's containment gate
  rejects the smaller host mapping before physical layout comparison, adds it
  to matches and materializes a new parent. The one-time SynchronizeGuest does
  not give retained child views persistent ownership/coherence with the parent.
- GetTexture can retain that earlier child in the TIC cache without invoking
  FindOrCreate again. Its same-channel-sequence fast path precedes replaced
  checking. Retained RT and display views also exist outside that TIC cache.
- Different GOB layouts correctly fail shared-image matching, but partial
  prefix spans do not enter the identical-mapping AliasGroup mechanism.

The containment path exists in **both** 381d52d3 and 3e8689f6. The synthetic
test proves an ownership gap, **not that this specific condition was introduced
by #341 or is the unique device cause**. #341 adds mip reuse validation and
alias machinery, but the actual failed device predicate remains unknown.

## Reproducer

Run `TEXMAN_HDR_PREFIX_ONLY=1 python3 tests/gpu_backing_promotion/run.py` with
the suite's documented host Vulkan/validation dependencies.

It uses the actual manager, matcher, synchronization methods and real Vulkan
images with the requested HDR format and exact physical spans. A retained
child is initialized, parent mip0 receives three render-pass clears with
different HDR values across completed submissions, and subsequent child/base
readbacks are compared byte-for-byte. Both creation orders and BH16/BH8 child
layouts are exercised. The incompatible case must never share an image.

Observed current-master results (UBSan + Vulkan fixture):

| Creation order | Child BH | Shared backing | Fresh on all 3 writes |
| --- | --- | --- | --- |
| Parent then child | 16 | yes | yes |
| Parent then child | 8 | no | no |
| Child then parent | 16 | no | no |
| Child then parent | 8 | no | no |

The test deliberately fails with `HDR prefix cached views have no persistent
coherence`. No fixed/green result is claimed. The host GuestTexture constructor
was corrected to calculate mip layouts from the descriptor's actual BH/BD/
spacing, matching the production constructor; it previously retained generic
BH1 metadata and produced a false failure even for parent-first/BH16.
The pre-#341 manager body was also run against the same fixture: the same
three incoherent cases fail. This compares actual manager bodies with common
current transport/layout fixtures, not complete historical Android builds.
The mapping/multirange and sampled-layout/alias suites pass. The complete
backing-promotion/alias suite still aborts with `Cannot losslessly reconcile
these texture alias layouts`, also observed on the prior unmodified baseline;
it is not reported as a successful full Vulkan suite.
The test does not yet certify mip1 preservation, reverse writes or alias
destruction for a future correction. Those acceptance cases remain required.

## Why no speculative correction is included

Simply marking children replaced misses retained direct/RT/display views and
the same-sequence TIC cache. Widening AliasGroup registration to unequal spans
requires preserving regional authority, including the parent's other mips.
Allowing existing promotion to use larger spans without migration breaks its
explicit mirror/stride bounds and mapping/trap identity guarantees.

For proven compatible layouts, a bounded canonical-identity expansion is a
candidate design: preserve the existing Texture, submit prior work, preserve
GPU-owned mips, transactionally expand guest mappings/mirror/trap/index, then
switch backing generation. For incompatible layouts, that cannot substitute
for persistent overlap coherence. Neither design is implemented or benchmarked
here. No brightness, gamma, shader, blending or LOD change is justified.

## Minimum missing device evidence

The temporary HDR_PREFIX_LAYOUT/MAP/MIP records report only newly allocated
640x360 B10G11R11UfloatPack32 images with one/two levels, bounded to 32 reports.
They include actual physical spans, declared BH/BD/spacing, stride, view bounds,
per-mip dimensions, offsets, effective BH/BD/spacing, GOB width and byte counts.
They add no GPU readback, submission, fence wait, upload or ownership change.
This is a targeted allocation diagnostic, not another broad capture stream.

Install the separately named **Strato HDR Prefix** package (.hdrprefix), choose
the same Turnip/settings with Free Guest Texture Memory OFF, cold boot and
reach the same Stadium scene. Return emulation.log; no capture flag is needed.
The record must contain both overlapping representations before choosing a
sharing versus incompatible-alias correction. APK/build success is not visual
validation and this APK is not expected to fix the glow.
