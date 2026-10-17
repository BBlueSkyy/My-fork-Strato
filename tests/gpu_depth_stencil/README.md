# Depth/stencil transfer layout and CPU benchmark

Run `python3 tests/gpu_depth_stencil/run.py`. The runner compiles the unchanged
production `Texture::GetBufferImageCopies`, the actual format names/widths, and
the directly included `depth_stencil.h` under ASan/UBSan. Texture allocation is
not performed here. The format fixture omits unused view-swizzle metadata and
the small-vector fixture uses a standard vector; GPU execution and the actual
guest CPU methods are tested in the neighboring suites.

Tests cover disjoint depth/stencil planes, tiny-mip alignment, one/two/three
layers, both D24 packed orders and D32S8, byte-exact round trips, independent
expected plane values, guard bytes and rejection of undersized staging buffers.
Single-aspect tests cover one/two/four-byte texels with odd mip/layer sizes.
`--ref 3065a7e3` is a negative control: the original aspect regions overlap.

`VULKAN_HEADERS_INCLUDE_DIR`, `CXX`, and `CPLUS_INCLUDE_PATH` can select portable
Vulkan headers. Normal Android source dependencies are not needed.

## Benchmark reproduction

```
taskset -c 0 python3 tests/gpu_depth_stencil/run.py --ref 3065a7e3 --benchmark
taskset -c 0 python3 tests/gpu_depth_stencil/run.py --benchmark
```

Choose an allowed CPU on the host and run the commands sequentially. Both builds
use GCC `-O2` without sanitizers. Color region generation uses 100,000 warmup
iterations and seven samples of 5,000,000 iterations. It exercises the exact
old/current production method with the same vector fixture; it is not a GPU,
draw, render-target cache, shader compiler or FPS benchmark.

The second output measures the *new correct conversion* of a 1920x1080 D24S8
surface, seven samples of ten unpack+pack round trips, without allocation inside
the timed loop. It is an absolute CPU cost, not a performance comparison to the
incorrect old upload/readback. `--ref` changes only the selected production
method/metadata; it deliberately does not replace the packing helper. Do not
report the old-ref helper result as master performing correct DS conversion.

Record distributions and run order. A shared host can produce large scheduling
and frequency variation; gameplay and shader compilation measurements require
the Android device.
