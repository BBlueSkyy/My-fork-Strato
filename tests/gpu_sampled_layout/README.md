# TIC sampled block-linear layout regression

Run `python3 tests/gpu_sampled_layout/run.py` with a C++20 host compiler. The
runner enables AddressSanitizer and UndefinedBehaviorSanitizer by default;
`SANITIZE=0` disables both, and `CXX` selects the compiler.

The fixture uses the actual production TIC definition, the complete dimensions,
texture-type and tiling construction block from `Textures::GetTexture`, the
production `GuestTexture` size/stride methods, and the production block-linear
copy implementation. Only Vulkan metadata and unrelated host resource interfaces
are facades. No Android/Vulkan runtime is needed.

An independent byte-address oracle fills guest storage using Maxwell's GOB bit
addressing rather than the production swizzler. Bytes outside the effective
sampled surface contain poison. The primary regression is 512x128 BC3, declared
BH16, effective BH4: the declared allocation remains 262144 bytes and the upload
must reproduce all 65536 packed bytes exactly. Before the fix, the actual upload
first differs at packed byte 64.

Controls cover compressed-row ceiling for odd BC3 and ASTC dimensions, RGBA,
array/cube declared strides, unchanged nonzero base array layers, unchanged
1D/3D/depth/interleaved-Z layouts, already-small Y blocks, `e2DNoMipmap`, unchanged mipmapped and pitch TICs,
and explicit low-level render-target/DMA Y block allocation/addressing. The last
control deliberately bypasses sampled TIC auto-sizing: it must retain BH16.

The fix follows texture-unit Y block auto-sizing, as documented by envytools
`docs/hw/memory/g80-surface.rst` and independently used by
Eden's `AdjustMipBlockSize` upload layout. It is deliberately limited to
single-mip 2D color TICs with Z block depth 1 and base array layer 0, matching
the layouts supported by the lossless alias bridge. Low-level layout functions
are unchanged.

The alias fixture compiles the complete production exact-overlap routing
predicate, alias eligibility/registration/write/reconciliation methods, and the
executor's alias retry handler. It uses a metadata-only resource facade: its
readback checks that pending work has been submitted, without claiming to test
Vulkan execution. A native RGBA 512x32 raw-BH16 render target and effective-BH4
TIC must request alias reconciliation before readback, preserve their shared
writer group, invalidate the cached TIC after a later render-target write, and
submit/reconcile again before sampling. Controls retain existing routing for
changed dimensions, mips, Z layout, depth aspect and nonzero base array layers.
Before the routing fix this fixture fails with `readback occurred before executor
submission`.

This synthetic regression establishes a generic sampled-layout defect. The
captured Naruto draw has no actual TIC dump confirming this oversized Y mode,
so the test does not establish that the change fixes Naruto on a device.

TIC width-spacing coverage additionally verifies BC1 1920x1080 (60-to-64 GOB
columns), odd BC3/ASTC and 3D planes against the address oracle in both copy
directions, including untouched padding. Four literal mip/array vectors check
pixel-first compression rounding, declared-Z eligibility and layer-end X
alignment. Slice-origin controls distinguish padded and unpadded plane strides.
The alias fixture verifies that physically identical RT/TIC spacing reuses its
backing and that different encodings enter coherent alias reconciliation.
