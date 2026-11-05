# PR #424 status — mixed region authority (experimental twin of #400)

Branch `experiment/mixed-region-authority-on-20261105`, forked from `dfa18b43` of #400.
`MixedRegionAuthorityEnabled` is **false** in production. Nothing here claims game
compatibility, device stability or performance. #400 is untouched.

## Crash cause (from code, not from device logs)

The `Unsupported mixed buffer/image physical layout with active authority` exception is
thrown by the experimental fallbacks (`3e1e0892` BufferManager, `e4aaaf70` TextureManager):
`Texture::RegisterMixedRegionAuthority` returned `false` and at least one participant was
already tracked, so the "legacy path" escape hatch was refused and the manager threw.
Path: `Texture::RequireAliasCoherence` → `BufferManager::RegisterTextureRegions`.

Why registration returns `false` (all of it before any state is mutated):

- Existing groups are expanded first, so **one** unsupported newcomer rejects the whole
  component, including images that were already fine.
- Image gate: native RGBA8 UNORM only, 1 mip, 1 layer, depth 1, optimal tiling, 1 sample,
  full-GOB spans only (no edge GOBs, `widthBytes % 64`, `rows % 8`), ≤128 spans, resident.
- Buffer gate: `isDirect` (DMI) rejected.
- These gates mirror real limits of the raw transport, not arbitrary caution:
  `PlanRawRgba8GobCopies` hard-codes 4-byte texels, 4x1 sector rects and mip 0/layer 0, the
  buffer→image barriers cover `{color,0,1,0,1}`, and a transfer is capped at 128 GOBs.
  Removing a gate without generalizing the transport would corrupt data.

The exact layout that crashed Dragon Ball Z: Kakarot is **not known**; no log was available.
sRGB RGBA8 is one rejected class that is common in games and is now handled (below); mips/
layers, other formats, edge GOBs, >128-GOB images and DMI buffers are still rejected.

## Fallback review

`legacyOnly` (both managers) lets an unsupported overlap continue on the image-only path when
nothing is tracked yet, then caches `lookupRevision`. No persistent relation is installed, so a
later GPU write to the buffer does not invalidate the overlapping image or vice versa: stale
reads, i.e. the I3 blocker in `region-stage-c-fallback.md`. The commits stay in history, but the
behaviour is now behind `MixedUnsupportedLegacyFallback{false}`. With it off, unsupported
overlap rejects explicitly (as in #400). At `e4aaaf70` with the gate enabled,
`CheckMixedAdmission` fails (`rejected` expected, nothing thrown); it passes now.

## Layouts now supported

| Layout | Evidence |
|---|---|
| RGBA8 UNORM (unchanged) | existing suite |
| RGBA8 sRGB, 2D, 1 mip/layer, whole GOBs, ≤128 spans, staged buffers | `CheckAutomaticMixed` through production managers + executor on real Vulkan (lavapipe): byte oracles for image→buffer, sub-GOB writes, exclusive bytes, CPU publication, 32 stable cached uses; Free Guest ON/OFF, format-list OFF/ON. Negative control: removing sRGB from `MixedRawFormatSupported` reproduces the exception. |

Images sharing one physical region must now be the same `vkFormat` (explicit rejection otherwise).

## Still rejected (explicit exception, no data dropped)

Other formats (BGRA8, 1/2/8/16-byte texels, BCn), mips, layers, 3D, edge GOBs, images with
>128 GOBs (most real textures ≥ 256x256 RGBA8), nonresident mappings, DMI buffers. Supporting
them requires a generalized planner (bytes/block, block extent, level/layer), barriers over
all subresources, and a larger-than-128-GOB transport; each needs its own byte-oracle test.
A coherent fallback for the remaining classes needs the staged guest-bridge in
`region-stage-c-fallback.md`; it is not implemented.

## Not done

- Sequencing/publication audit beyond re-running the existing gates: no new defects found,
  none fixed; no new lock-order or Free Guest analysis.
- SpongeBob texture corruption: not investigated (no evidence it involves this path).
- Super Mario Odyssey, Zelda, FighterZ: no device data. No benchmark run (`gpu_resource_benchmark`).

## Test evidence (host)

Clang 18 compile, GCC ASan/UBSan runtimes, Vulkan-Headers 1.3.275, lavapipe, `LP_NUM_THREADS=2`.
**The Khronos validation layer is not installed here**, so runs used `TEXMAN_NO_VALIDATION=1`:
no synchronization validation. The "zero synchronization validation errors" line printed by
the suite is unconditional and is not evidence in these runs.

| Run | Result |
|---|---|
| `gpu_buffer_image_sequence/run.py --automatic` (e4aaaf70, gate on) | FAIL in `CheckMixedAdmission` |
| same, after this branch's changes | PASS (incl. C1/C2 coalesce, drain, failures/retry, CBUF/DMA mapping) |
| `TEXMAN_AUTHORITY_PROBE=1 gpu_backing_promotion/run.py` | PASS, A→B→reuse A 0/1024 stale texels |
| `gpu_region_executor`, `gpu_region_residency`, `gpu_texture_sequence`, `gpu_overlap_materialization` | PASS |

## Activation

Not reactivated. Required first: generalized layouts or a tested coherent fallback for the
rejected classes above, a run with the validation layer, and a device pass on the Moto G100.

## Guest-bridge byte coverage groundwork (45fe79d4 / 23cf681a–092f29b3)

Added a bounded standalone **native 2D encoder-byte coverage planner** and
physical-mapping resolver in `texture/guest_bridge_coverage.h`, exercised by
`tests/gpu_guest_bridge_coverage/run.py`. This code is independent of the mixed
authority release gate. The byte oracle covers edge sectors, mip/layer gaps,
tile-width-spacing padding, discontiguous and reversed physical mappings,
self-alias rejection, overflow/work budgets, and an image larger than 128 GOBs.

`Texture::PlanGuestBridgePhysicalCoverage()` converts a real `GuestTexture`
into per-level/layer native byte runs using production `GetBlockLinearMipLayout`
and `GetBlockLinearWidthGobs`, then resolves those runs through resident mappings.
This is a **preflight only**: it does not register mixed authority, migrate writer
versions, publish guest bytes, repair stale resources, or bypass direct transport
restrictions. Native reversible 2D layouts are only potential candidates; the
special 12-byte encoding and unsupported layouts are excluded.

The branch's `pr_build.yml` adds a byte-oracle check before Android assembly.
CI and full Vulkan synchronization validation must be confirmed separately;
**do not represent an uncompleted run as PASS**.

**I3 remains unresolved.** The next increment must install a transactional,
persistent region-writer/consumer graph and selective guest publication, not
flip `MixedUnsupportedLegacyFallback` or enlarge direct GOB admission without
matching transport changes. Both flags remain `false`.
