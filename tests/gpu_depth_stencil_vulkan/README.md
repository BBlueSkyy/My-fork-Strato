# Depth/stencil headless Vulkan integration

Run `python3 tests/gpu_depth_stencil_vulkan/run.py` from the repository root.
Requires a C++20 compiler, Vulkan C/Hpp headers and loader, a graphics/compute
Vulkan ICD, `VK_LAYER_KHRONOS_validation`, `glslangValidator`, and `spirv-val`.
Mesa lavapipe works without a display. Standard `VK_ICD_FILENAMES`,
`VK_LAYER_PATH`, `CPLUS_INCLUDE_PATH`, `LIBRARY_PATH`, and `LD_LIBRARY_PATH`
can select portable dependencies. `CXX`, `VULKAN_HPP_INCLUDE_DIR`,
`VULKAN_HEADERS_INCLUDE_DIR`, and `VULKAN_LOADER_LIBRARY` override build inputs.
Missing dependencies fail the runner; the test does not silently skip.
Set `DEPTH_STENCIL_BASELINE_COPIES_REF` to a Git revision to replace only
`GetBufferImageCopies` with that revision's unchanged method as a negative
control; the original overlapping aspect offsets must fail the data check.

The runner extracts the current, unchanged `Texture::GetBufferImageCopies`,
`CopyFromStagingBuffer`, and `CopyIntoStagingBuffer` methods. A minimal test
Texture exposes their real fields; allocation and scheduling are test facades
using real Vulkan resources, command buffers, queues, and fences. Production
`depth_stencil.h` is included directly. No Android device or emulator frontend
is required or claimed.

Coverage:

- D24S8 and S8D24 guest physical component orders on Vulkan D24S8, plus D32S8.
- Distinct depth/stencil texels across four mips and three array layers,
  including one-texel mips with odd layer counts and aligned stencil offsets.
- Packed upload, production aspect copy regions, GPU image data, production
  image readback, and packed output checked byte for byte.
- Repeated uploads while already in GENERAL, without an intervening readback.
- Depth-only and stencil-only clears on one mip/layer, with every other aspect
  and subresource verified unchanged.
- A real render-pass depth attachment clear with stencil LOAD preservation,
  immediately followed by a compute shader sampling the depth attachment and
  checking shader output and packed image readback.
- D16-only and S8-only transfers with odd dimensions, three mips and layers,
  verifying aligned offsets and exact packed roundtrips.

The device requests Vulkan 1.1 without optional device extensions or features.
All depth/stencil image layouts are combined layouts; separate depth/stencil
layouts are not enabled. Synchronization validation is explicitly enabled and
any validation error fails the executable. Fences wait only for completed test
readback; no device/queue idle waits are used. Shader SPIR-V is validated for
Vulkan 1.1 before the executable is built.

This test exercises the transfer methods and attachment/sample consumers, not
the complete Android `SynchronizeHost`/`SynchronizeGuest`, guest memory trap,
blocklinear swizzle, or emulator command-executor lifecycle. Those CPU paths
need separate production-method tests. It is not a broad GPU conformance suite.
