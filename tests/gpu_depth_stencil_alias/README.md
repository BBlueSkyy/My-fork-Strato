# Packed D24 alias coherence

`python3 tests/gpu_depth_stencil_alias/run.py` compiles the production format
compatibility predicate, full TextureManager lookup, alias registration,
reconciliation, writer publication, memory pins and guest/host transfer methods.
It adapts only the existing backing-promotion test's allocator/scheduler/platform
boundary. Vulkan images, transfer commands, clears, fences and synchronization
validation are real. Existing backing-promotion fixtures are not changed or run.

The test uses two encodings of the same exact block-linear guest mapping: depth
in the low 24 bits with stencil in the high byte, and stencil in the low byte
with depth in the high 24 bits. It proves distinct coherent host images,
alternating GPU writers, lossless byte-mediated refresh, same-format backing
reuse, lease release and no extra allocations/submissions on repeated reads.
D32S8 padding, pitch and 3D layouts remain excluded from the lossless DS bridge.

Requires a C++20 compiler, Boost headers, Vulkan-Headers/Vulkan-Hpp, loader, ICD
and Khronos validation layer. As with the backing-promotion runner, override
`CXX`, `BOOST_INCLUDE_DIR`, `VULKAN_HPP_INCLUDE_DIR`,
`VULKAN_HEADERS_INCLUDE_DIR`, and `VULKAN_LOADER_LIBRARY` when needed.

The initial test run before the fix failed at the production predicate's
opposite-order compatibility assertion, establishing the regression's cause.
Reproduce that negative control with `DEPTH_STENCIL_BASELINE_COMPAT_REF=3065a7e3`
when running the same command; only the actual compatibility predicate is replaced.
