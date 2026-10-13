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

## Part 1: incompatible guest representations

When exact ordered guest mappings cannot share a legitimate backing, valid
native 2D block-linear color representations can retain separate images. Each
layout must fit its mapping and stride, remain losslessly encodable, and satisfy
the existing staging alignment. Converted formats, discarded guest bytes,
partial mapping overlaps, 3D reconstruction and pitch/linear reconstruction
remain guarded. Representable mip growth still takes the canonical path above.

An alias group records the latest real GPU writer, its generation and the last
generation encoded into guest bytes. Existing render-target and explicit writer
hooks publish ownership; looking up or reading a representation does not become
a write. Registration flushes only the newest writer, including when a newer
writer has already completed its readback. All superseded representations are
stale and cannot overwrite guest memory through synchronization, CPU traps or
destruction.

A stale cached view requests an executor retry before recording the operation.
The existing submission boundary releases resource locks; the authoritative
writer is then synchronized through its own guest encoding, and the target is
uploaded using its own layout. The writer is downloaded once per generation;
repeat reads need no extra upload or backing reconstruction. A CPU write arriving
between sampled attachment preparation and submission retains the original
submit-time CpuDirty upload. Non-executor lookups and retained display views
reconcile before taking their texture lock.

The guest constructor no longer installs callbacks before shared ownership
exists: the manager already installs them after construction. Trap callbacks
use nonblocking alias-operation locks while the trap lock is held. Pending
registration temporarily pins the existing guest mappings using the same lease
mechanism as mip growth; the FreeGuest implementation is unchanged.

Part 1 does not change runtime Vulkan copy/barrier methods, storage-image
binding/cached-write policies or Free Guest Texture Memory policy. It is not an
optimization: a change of representation after GPU writes uses the existing
synchronous guest readback and can incur a wait. Android/game behavior requires
device testing; this does not establish that RE5's black graphics are resolved.

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

### Part 1 ownership tests

The host suite additionally compiles the full production FindOrCreate, alias
registration/reconciliation, attachment/retry, dirty-state synchronization and
trap callback bodies. Real Vulkan render-target writes and byte readbacks cover
90112-byte/six-layer and 180224-byte/three-layer layouts over the same 540672-byte
allocation, a third 270336-byte/two-layer layout, alternating writers, stale
readback rejection, CPU writes, allocation/recording failures, stable cached
views and repeated reads with unchanged upload/submission/allocation counters.
It also checks canonical one-to-eight mip growth while another layout is cached.

Vulkan transport (CopyFromStagingBuffer, CopyIntoStagingBuffer and
TransitionLayout) is an explicitly isolated host fixture for these tests. Its
test-only dependencies allow the Part 1 byte/ownership assertions to run without
implementing the additional runtime barriers reserved for Part 2. Zero validation
errors in this fixture do **not** certify the unchanged runtime dependencies.
Android allocation, scheduler and memory mirror interfaces remain host adapters.

`TEXMAN_BASELINE_MANAGER_REF=956ad8f6` substitutes the published manager body;
the distinct-stride creation fails with the original unrepresentable-layout
exception. The current body passes. The completed-readback registration and
pre-submit CPU-write tests were also observed failing before their fixes.
