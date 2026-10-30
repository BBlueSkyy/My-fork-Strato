# Fully sparse sampled 2D textures

This change selectively adapts PR #373 (`9296e22b`) onto master
`9935f13349c00d732ae599f4f90866b5b79629f5`. It does not merge #373 or
incorporate #393, #394, #395 or #396.

## Eligibility and cache

Only the failed `GetTexture` translation path can consult the zero-image cache.
The entire requested guest byte range must still consist of sparse mappings
under one GMMU lock. Null spans alone are insufficient: the translator also uses
them for genuinely unmapped holes. A resident byte, an unmapped byte, an empty
range or an overflowing range rejects the sparse fallback. The split translation
path no longer exposes sparse placeholder addresses to consumers or callbacks.

Eligible descriptors are sampled, noninteger color 2D textures, one layer, no
base array layer, depth one, legal mip chain and a nonempty in-bounds mip view.
There is no hardcoded 1024-pixel / 11-mip ceiling. Device limits and the memory
budget determine whether a descriptor can be admitted.

Each `(width, height, mip count)` shares one read-only zero image. Views retain
the TIC's mip base/count and swizzle. RGBA8 is an internal numerical-zero
representation, not a conversion of resident guest textures or compressed
blocks. Guest format defaults are composed with the swizzle (BC5/R/RG/RGB
supply absent alpha as one; explicit Zero/One selectors remain explicit).
Nothing is uploaded from a sparse placeholder. The implementation makes no
claim about decoding an arbitrary all-zero compressed block.

The sparse views are never inserted into the positive TIC cache or physical
TextureManager. Every subsequent lookup still attempts GMMU translation before
consulting this cache, so a physical remap can recover the real texture.
The normal resident lookup/cache path is unchanged.

## Memory and Vulkan ordering

The logical RGBA8 size is the checked sum of
`4 * max(1, width >> mip) * max(1, height >> mip)` for every declared mip.
The 12-level chains are:

| Dimensions | Logical RGBA8 bytes |
| --- | ---: |
| 2048 x 2048 | 22,369,620 |
| 512 x 2048 | 5,592,412 |
| Combined | 27,962,032 |

The additional image allocation budget remains 32 MiB per `Textures` instance.
Admission also checks the host image format's extent, mip, layer, sample and
resource-size limits, the core Vulkan 1.1 maximum memory-allocation size, and
actual optimal-tiled image memory requirements. An unbound image probes those
requirements before allocating; VMA's reported image allocation size is charged
against the budget. Driver tiling/padding can therefore reject an image that
fits in logical bytes. VMA retains its normal handling of device-memory pressure
and allocation-count limits; allocation errors are not turned into silent zeros.

A newly allocated image uses one command submission: undefined -> general with
transfer-write access, clear all mips, then transfer-write -> shader-read with
an all-commands destination stage. Its fence cycle retains the image and is
consumed by the existing executor ordering. Cache hits never clear or allocate.
No changes to the Vulkan backend or allocation policy were needed.

Unhandled mapping errors still warn for each distinct TIC, first occurrence and
then powers of two, with an occurrence count. Translation is retried on every
lookup regardless of log suppression. PR #373's mapping/remap diagnostics and
its workflow test exceptions were not ported.

## Independent Oceanhorn 2 termination

The supplied #373 log `emulation(20261025-114534).log` records:

- `SetTerminateResult: 3276802 (module 2, description 6400)` at line 61084.
- `Guest svcBreak: reason=0x0 ... size=0x4` immediately afterward.

`3276802 == 0x320002 == (6400 << 9) | 2`. In this repository,
`services/fssrv/results.h` names that value `PermissionDenied`.
`fssrv/helpers.h::MapVfsError` can produce it for permission-denied,
operation-not-permitted or read-only-filesystem errors. Atmosphere's reference
FS result definitions independently place description 6400 in PermissionDenied:
https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libvapours/include/vapours/results/fs_results.hpp

`IApplicationFunctions::SetTerminateResult` only records the guest-supplied
result; `svcBreak` requests guest termination. The supplied log does not identify
the filesystem command/path that originated the result, and a matching numeric
result alone does not prove which call failed. There is no proven causal link
to sparse sampling or device loss. Filesystem behavior and termination handling
are intentionally unchanged; this change does not claim to fix that crash.

## Validation and limits

- `python3 tests/gpu_sparse/run.py`: exact production split-translation and
  whole-range query bodies under ASan/UBSan; mixed/hole/remap cases; resident
  coalescing; checked byte sums, NPOT/tail levels and invalid mip views.
- `python3 tests/gpu_sparse/run_vulkan.py`: exact initialization function compiles
  against the pinned Vulkan-Hpp/VMA headers; production swizzle helper is executed
  with BC5, R8, RGBA8, ASTC and explicit Zero/One selections.
- `python3 tests/gpu_sampled_layout/run.py`: existing production-code layout,
  alias, upload/readback and TIC-construction regressions.
- `git diff --check`.

The pointer bug and BC5-alpha bug were reproduced as failing assertions before
the respective corrections. The Vulkan check uses a structural host fixture;
it is not a full Android build or a live-device execution test. This environment
has no Android SDK/NDK. Full ARM64 DevRelease compilation and artifact upload
use the unchanged PR Build workflow. No tests or other workflows were disabled.
Actions are not monitored after publishing the Draft PR.

Integer, depth/stencil, array/cube/3D, partially resident, unmapped and over-budget
textures retain the existing failure path. This is not general sparse-resource
residency support, and no on-device graphics, performance or crash result is
claimed before testing the APK.
