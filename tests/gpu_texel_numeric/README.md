# Sampled texel buffer numeric types

These host tests check Vulkan correctness and cache behavior. They do not establish
that SpongeBob's visual corruption is fixed on Android.

Initialize the pinned shader-compiler, fmt, range, sirit and SPIRV-Headers
submodules. Install a C++20 compiler, Boost headers, SPIR-V Tools, Vulkan headers,
a software Vulkan ICD, Khronos validation layers and libxxhash. The backend and
Vulkan tests run with ASan/UBSan. Disable leak detection only on hosts that cannot
support LSan (`ASAN_OPTIONS=detect_leaks=0`).

```sh
python3 tests/gpu_texel_numeric/build.py --deps app/libraries --build-dir /tmp/texel-host
python3 tests/gpu_texel_numeric/bundle.py
python3 tests/gpu_texel_numeric/views.py
c++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all tests/gpu_texel_numeric/vulkan.cpp -lvulkan -o /tmp/texel-vulkan
/tmp/texel-vulkan /tmp/texel-host
```

`--boost-include`, `--spirv-tools`, `CXX`, `CXXFLAGS`, `CPLUS_INCLUDE_PATH`,
`XXHASH_LIBRARY`, `VK_ICD_FILENAMES` and `VK_LAYER_PATH` support nonstandard host
installations. No Android SDK is required for these tests.

The emitted compute module writes all four components of Float/UINT/SINT fetches
back to an SSBO as raw words. The Vulkan test binds float32, UINT16/SINT16,
UINT32/SINT32, UNORM16 and UINT8/SINT8 views and checks 36 exact words. It covers
zero/sign extension, integer alpha=1, float bits, normalized float conversion and
suppression of relaxed integer precision. The fragment version also passes
SPIR-V Tools. `views.py` executes the production key/hash/store bodies through
format, offset, range, backing and owner changes with host object facades; it is
not a GPU lifetime proof. `bundle.py` executes production serialization, reset
and version checks: v5 and the historical experimental v6 are rejected by v7.

The negative control recreates the original all-float declaration while retaining
the same valid views:

```sh
python3 tests/gpu_texel_numeric/build.py --deps app/libraries --build-dir /tmp/texel-negative --baseline
/tmp/texel-vulkan /tmp/texel-negative negative
```

It must produce exactly two `VUID-vkCmdDispatch-format-07753` errors. The invalid
command buffer is **never submitted**. SPIR-V Tools alone cannot detect a mismatch
between a shader and its runtime view.

The compiler/runtime single-source shift regression is covered by the related
TexturePass and TIC tests, including source=3 shifted by2 (expected handle12).

Related directed regressions:

```sh
python3 tests/gpu_tic_validation/run_shader.py --deps app/libraries
python3 tests/gpu_tic_validation/run.py
python3 tests/gpu_tic_validation/run.py --probe-cache --vulkan-lifetime
python3 tests/gpu_tic_validation/preload/run.py
python3 tests/gpu_tic_validation/replay/test_query_callbacks.py --deps app/libraries
python3 tests/gpu_sampled_layout/run.py
```
