# Backing promotion verification

Initialize the pinned Vulkan-Hpp and Boost submodules (including Boost's children).
Use Vulkan C headers matching Vulkan-Hpp's `VK_HEADER_VERSION`; this branch's
pinned Hpp is version 202. Set `VULKAN_HEADERS_INCLUDE_DIR` if those headers live
outside `app/libraries/vkhpp/Vulkan-Headers/include`.

```sh
python3 tests/gpu_framebuffer_cache/run.py
python3 tests/gpu_draw_texture/run.py
python3 tests/gpu_backing_promotion/run.py
```

The last command requires
`VK_LAYER_KHRONOS_validation`, an ICD with BC compression, image-format lists and
imageless framebuffer support. Set `VULKAN_LOADER_LIBRARY`, `LD_LIBRARY_PATH`,
`VK_ICD_FILENAMES` and `VK_LAYER_PATH` when using a private Vulkan installation.
The test enables synchronization validation as well as ordinary validation.

Promotion tests compile the actual production function bodies unchanged against
host allocator/scheduler/fence interfaces and real Vulkan resources. They do not
replace the image migration with an imitation. They deliberately prohibit image
move assignment to catch duplicated allocation ownership. Failure paths must
leave backing/generation intact and retain evicted framebuffers for old work.

Readbacks exist only in the test to compare every byte of all mips/layers. They
are not part of the runtime policy. These checks establish host-side Vulkan
legality and migration semantics, not mobile-driver performance or a complete
Android build. Device acceptance is described in
[`docs/selective-mutable-format-audit.md`](../../docs/selective-mutable-format-audit.md).

Mip expansion additionally compiles the actual full-match predicate/selection,
executor retry handler, guest-memory pin/free methods, mip-layout calculation,
and block-linear conversion. A 128x128 native 2D array with one mip and six
layers is expanded to an eight-mip cube with an unchanged 90112-byte guest
layer stride. Byte readbacks cover every mip and face, both GPU-owned stale
guest mip0 and CPU-owned mip0, block heights 1 and 16, and subsequent writes
through the original cached alias. The test checks mapping identity, view
generation, upload cycle dependencies, guest-byte retention during submission,
download-buffer reset, and allocation failure. Guest synchronization and OS
memory discard remain explicitly identified host interfaces; their upload
commands and the entire backing migration use real Vulkan resources.

`tests/gpu/texture_view_range_tests.cpp` also covers range overflow, remaining
count sentinels, cube shape/layers, view/image type mismatches and 3D depth-slice
views. See [`docs/texman-subresource-reuse.md`](../../docs/texman-subresource-reuse.md)
for the captured evidence and the boundary of this correction.

As a negative control, `TEXMAN_BASELINE_VIEWS_REF=c7cb28d8` loads only the two
view API bodies from the base commit. Running the same Vulkan test must fail
at the illegal eight-mip view and report VUID 01718. Without that environment
variable the test compiles the current implementation and must pass with zero
validation errors. Run the negative control only with a host software ICD.

Part 1 alias tests compile the complete production manager, ownership methods,
dirty-state synchronization and trap callbacks. They exercise three distinct
strides over identical mappings, alternating render-target writers, stale
readback prevention, repeated reads, canonical mip growth, registration after
a newer completed readback and CPU writes between attachment and submission.

CopyFromStagingBuffer, CopyIntoStagingBuffer and TransitionLayout are explicit
**test-only Vulkan transport fixtures** in this suite. Runtime copies/barriers
and FreeGuest policy remain unchanged by Part 1. Thus zero fixture validation
errors establish ownership/content behavior with synchronized transport, not
validation of runtime dependencies reserved for Part 2.

Negative manager control: `TEXMAN_BASELINE_MANAGER_REF=956ad8f6 python3
tests/gpu_backing_promotion/run.py` must fail at distinct-stride creation.

Part 1B additionally reproduces exact guest mappings containing a historical
host representation that fails CanReconcileGuestLayout while a valid new
stride is requested. Grouped and ungrouped history, executor and direct lookups,
newer GPU writers before/after readback, and a historical full-match candidate
are covered. Retirement must not encode stale bytes or revive retained cached
views/writers; a non-reversible authoritative GPU writer remains rejected.
A failed authoritative readback must leave both writer ownership and historical
entries intact for retry. Existing three-alias and repeated-read counters remain
asserted. No runtime Vulkan transport or FreeGuest policy changes are included.
The all-history/no-eligible-member case and a converted writer marked Clean by
skipped readback are also covered: only an actual completed guest encoding
permits retirement of the latest GPU writer.


## Unpublished physical-region checkpoint

See [the checkpoint and blocking concurrency test](../../docs/region-authority-checkpoint.md).
`TEXMAN_AUTHORITY_PROBE=1 python3 tests/gpu_backing_promotion/run.py` now asserts real Vulkan shared/exclusive content, alternating writers, failure recovery, CPU callbacks, retained views and dense mip/layer regions.
`TEXMAN_LOCK_ORDER_PROBE=1 python3 tests/gpu_backing_promotion/run.py` is currently RED: operation-spanning component locking can invert presentation's resource/component order. Passing the pixel oracle alone does not authorize publication. These are optional host commands; APK workflows are unchanged.
