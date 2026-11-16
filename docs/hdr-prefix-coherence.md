> Historical foundation retained from PR451. For this Draft’s additional donor consolidation and current verification, see [compatible mip history coherence](compatible-mip-history-coherence.md).

# Compatible mip-prefix expansion

Experimental correction based on master `505bc7d1`. Device graphics acceptance
is still pending. PRs #400, #445, #448, #449 and #450 remain separate.

## Confirmed device condition

The PR #450 log records allocation of the child before the parent at
`0x86BB4D000` (and repeats the same pattern at `0x86BA4D000`):

| Property | Child | Parent |
| --- | --- | --- |
| Dimensions / format | 640x360 B10G11R11UfloatPack32 | Same |
| Mips | 1 | 2 |
| Mapping length / layer stride | 983040 | 1310720 |
| Block height / depth / spacing | 16 / 1 / 0 | 16 / 1 / 0 |
| Mip 0 offset / bytes / width GOBs | 0 / 983040 / 40 | Same |
| Mip 1 offset / bytes / width GOBs | — | 983040 / 327680 / 20 |

Earlier GPU captures identify the two-mip object as the scene writer
(`42016CE58CA1A259`) and the independent one-mip object as the bloom input
(`3F6EAFE7F26D6292`). Four input readbacks remain byte-identical while the
parent receives later writes. The later bloom upsample and composition are
`05EB6F31815A3BF8` and `F8F8901DE46A8AFF`; neither shader is changed here.

`FindOrCreate` rejects a smaller existing mapping at its containment gate,
before the mip-layout matcher. Creating the larger request then performs at
most a one-time guest synchronization and creates an independent Texture.
Only identical complete mapping sequences enter `RegisterAliases`; a prefix
is intentionally not an exact mapping. Retained TIC/RT/display TextureViews
can therefore continue to address the old child indefinitely. Marking it
`replaced` alone would not repair retained views or the same-sequence TIC cache.
Existing `CanGrowMipLevels` also correctly rejects growth outside the original
mirror/stride. Relaxing that bound would read beyond the child's mapped bytes.

## Historical attribution remains open

The containment/retained-view gap exists in both `381d52d3` and `3e8689f6`.
The original retained-child reproducer fails with both manager bodies.
The new log proves the current device takes that condition, but does not prove
that #341 introduced the gap or that it alone explains the visual transition.

A second experiment adds a 320x180 single-mip RT request at parent+983040:

| Manager body | Cached child is parent | 320x180 RT uses parent mip 1 |
| --- | --- | --- |
| `381d52d3` | no | no (independent image, mip 0) |
| `3e8689f6` | no | yes |

The #341 mip-loop fix advances the physical offset/level even when the current
offset does not match; the old loop made nonzero mip offsets unreachable.
This proves a relevant routing change. It does **not** prove its complete
interaction with MK8's earlier writes/cache history. These experiments use the
actual historical manager bodies with a common current Vulkan/transport fixture,
not complete historical Android executions. Do not label this Draft a proven
fix of the original #341 visual regression until the device comparison passes.

## Bounded correction

`CanExpandGuestPrefix` admits one contiguous, unaliased native 2D single-layer
block-linear allocation, starting at the same address, whose complete old mip
chain exactly ends at the old mapping boundary. Every old mip must retain its
dimensions, sizes, effective block geometry and GOB row width. Formats, ranges,
strides and complete mapped bounds are checked. Different BH8/BH16 encodings
remain incompatible even though this mip 0 has the same physical byte count.
Equivalent spacing is allowed only when it produces the same geometry.
Other overlapping histories, alias groups, freed memory, converted formats,
arrays, 3D images and invalid block heights are excluded.

The existing Texture identity is retained. The executor submits its old nodes
before a retry. The manager prepares the updated overlap index; promotion
prepares a larger mirror, image, staging and command resources, protects the
extension, snapshots only newly represented mips, copies the existing GPU mips,
and commits mapping/backing/index metadata. Old backings, raw views and
framebuffers remain fence-owned. Existing view objects keep their original
ranges and refresh their Vulkan handles through `backingGeneration`.

There is no per-frame overlap readback, ownership bridge or reconstruction.
Expansion is a one-time operation, not a game or brightness policy.

Safe temporary-trap rollback requires three small general fixes, covered by
real mmap/mprotect tests: deletion must recompute protections at overlapping
trap boundaries; teardown must not allocate; a fault already observed before
waiting on a resource must retry if its trap was removed during the wait.
Trap registration also reserves entry capacity before publishing its group,
so an allocation failure cannot leave an unreachable registration. Exact aliases
and single traps retain one overlap pass and one mprotect; no page-count loop
is introduced. These changes affect a shared subsystem and need device stress
coverage in addition to the new tests.

## Verification and limits

Run with an installed Vulkan ICD and synchronization validation:

```sh
TEXMAN_HDR_PREFIX_ONLY=1 python3 tests/gpu_backing_promotion/run.py
python3 tests/trap_prefix_rollback/run.py
python3 tests/gpu_texman_multirange/run.py
python3 tests/gpu_sampled_layout/run.py
```

The prefix test checks both creation orders, CPU/GPU ownership, a native command
recorded against the old image and submitted at the retry boundary, existing
views, changing HDR values over successive submissions, writes through both
views, unchanged image-allocation count after expansion, mip 1 preservation,
view destruction, incompatible layouts, preparation failures and retry. Mirrors
in these new cases use real shared memory. Allocator, scheduler and Android
process/trap interfaces remain host adapters; a separate test extracts the real
TrapManager/IntervalMap and uses real OS protection.

Local verification: the complete backing-promotion/alias suite passes with
UBSan and zero Vulkan validation errors (image-format-list both disabled and
enabled). Mapping/multirange, sampled-layout, trap rollback, framebuffer-cache
and draw-texture checks also pass. The host fixture was corrected to model
the descriptor's actual format/GOB layout, explicit requested-layout registration
for freed writers, discarded padding, and cancellation of state-only unsubmitted
command buffers. Those are test fixes, not alias-runtime-policy changes.

Negative control:

```sh
TEXMAN_HDR_PREFIX_ONLY=1 TEXMAN_BASELINE_MANAGER_REF=505bc7d1 \
  python3 tests/gpu_backing_promotion/run.py
```

It must fail at the retained child/parent identity assertion. Historical routing
can be repeated with `TEXMAN_HDR_ROUTING_ONLY=1` and manager references
`381d52d3` / `3e8689f6`.

Android builds use ARM64 DevRelease, signed for installation, with separate
package `org.stratoemu.strato.hdrprefixfix` and label **Strato HDR Prefix Fix**.
The workflow verifies nonempty APK/native AArch64 libraries before artifact
upload and errors if the APK is absent. Build monitoring is intentionally not
performed, as requested; no artifact existence or Android success is claimed
before the workflow completes.

Required device acceptance: Moto G100, same Turnip driver, Free Guest Texture
Memory OFF, cold boot and the same Mario Kart Stadium scene. Compare the pink
kart/asphalt bloom and right-side white glare at comparable race moments. Also
test SMO (fog/menu return), BOTW, RE5, repeated scene changes and CPU/GPU memory
pressure. No FPS/frametime, memory or stability guarantee is inferred from host
Vulkan tests. The historical trigger and visual result remain open.
