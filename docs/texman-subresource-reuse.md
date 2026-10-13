# Texman subresource reuse

Base: master `c7cb28d822c52437ed83e9f86ed15c3a3b77c6e7`, verified on 2026-10-13.
This change contains no game-specific policy or instrumentation and leaves
[PR #337](https://github.com/BBlueSkyy/My-fork-Strato/pull/337) unchanged.

## Existing evidence

The provided RE5 captures contain `draw_bindings_008.txt` with fragment shader
`F3805B306B15F89A`, Vulkan binding 21, in both executions:

| Property | Black graphics capture | Working graphics capture |
|---|---|---|
| Sampled view | Cube, RGBA8 UNORM, 128x128 | Cube, RGBA8 UNORM, 128x128 |
| View range | mip 0+8, layer 0+6 | mip 0+8, layer 0+6 |
| Stored guest representation | Cube, eight mips | 2DArray, one mip |
| Recorded mip layouts | Eight | One |
| Guest layer stride | 90112 bytes | 90112 bytes |
| Translated mapped size | 540672 bytes | 540672 bytes |
| State | GPU dirty, previously an RT | GPU dirty, previously an RT |
| Image flags / layout | CubeCompatible / General | CubeCompatible / General |

These are captured descriptor/resource metadata. The host image mip count was
not directly captured. The allocation code initializes `Texture::levelCount`
from the guest mip count and passes it unchanged to `VkImageCreateInfo`. The
full-match predicate ignored mip/layer bounds. Neither cached views nor format
promotion increased that count. Thus the one-mip representation can reach
Vulkan with an eight-mip view; this violates subresource-range limits
(VUID-VkImageViewCreateInfo-subresourceRange-01718).

The invalid configuration appears in the working capture. This proves a Texman
invariant defect, **not the cause of RE5's black graphics**. Matching shader
hashes do not establish shader/input correctness, and sampled GPU contents and
effective LOD were not captured. Free Guest Texture Memory was disabled in the
reported runs and is not used as a causal explanation.

## Correction

Full matches require compatible image geometry/type, tiling, layer stride and
representable view ranges. Partial mip/layer matches also validate their entire
range before selecting a parent. Both view APIs validate actual backing mip,
layer and image-type limits; cube and 3D depth-slice rules are checked separately.
Existing format/aspect normalization is preserved.

For an exact native 2D block-linear allocation with unchanged layer addressing,
the canonical `Texture` can expand its backing when the complete guest mip chain
fits the original layer stride and mappings. Existing mip layout prefixes must
match byte for byte. The captured eight-mip chain occupies 89088 bytes per face,
within its 90112-byte stride. Mips are neither clamped nor generated.

The executor submits the old nodes only when this backing change is needed and
restarts descriptor preparation. Existing GPU-owned mips are copied on the GPU;
CPU-owned existing mips are uploaded first. Added mips are deswizzled from their
guest bytes and uploaded to every layer. Separate source/destination barriers
cover the old/new chains, and dependencies retain staging, old images, views
and retired framebuffers. No queue/device-idle or GPU completion wait is added
to the expansion path.

All cached aliases keep the same `Texture`, mirror, mappings and trap identity.
Backing generation invalidates their Vulkan views. Mip layouts/sizes and guest
mip metadata are committed together after successful submission; the old
download buffer is discarded because its capacity is no longer sufficient.
A scoped guest-memory lease prevents reserved, not-yet-represented mip bytes
from being discarded while the executor submits the old representation. It
does not assume the setting caused the captured defect.

This is not a general solution for differently tiled/strided aliases. A newly
rejected parent that cannot be expanded without changing its byte layout fails
explicitly before invalid view creation or unsynchronized overlap readback.
Arbitrary layer expansion, converted host formats, already discarded guest
tails and reconstruction of unrelated GPU alias writers are not implemented.
The older incompatible-format overlap reconciliation remains outside this fix.

## Local validation

`tests/gpu/texture_view_range_tests.cpp` checks the captured 1-versus-8 case,
overflow, remaining-count sentinels, layer bounds, cube shape/count, image type,
and mip-specific 3D depth slices.

`tests/gpu_backing_promotion/run.py` compiles production method bodies and the
actual full-match predicate/selection against real Vulkan images, buffers,
queues and fences, with Khronos ordinary and synchronization validation. Host
interfaces replace Android allocation/scheduling/traps; the backing migration,
layout calculation, block-linear conversions, range checks, memory lease,
retry handler and view creation are production code. Guest synchronization and
OS memory discard are explicitly identified host interfaces.

The integration test compares all 524280 bytes of eight mips and six faces,
with deliberately stale guest mip0 for GPU-owned data and a separate CPU-owned
case. It exercises GOB block heights 1/16, format-list support on/off, later
writes through the original cached alias, unchanged mapping identity, view
generation, upload dependencies, allocation failures, and download-buffer
reset. Existing format-promotion, framebuffer lifetime, BC1/BC3 and DRAW_TEXTURE
tests remain applicable. Host validation does not establish Android driver
behavior or whether RE5's visible defect is resolved.
