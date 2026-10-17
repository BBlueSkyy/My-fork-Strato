# Production guest depth/stencil synchronization

Run `python3 tests/gpu_depth_stencil_guest/run.py`. Requires a C++20 compiler
with ASan/UBSan and Vulkan C/Hpp headers. No Vulkan loader or GPU is used here.
`CXX`, `CPLUS_INCLUDE_PATH`, and `VULKAN_HPP_INCLUDE_DIR` select dependencies.

The runner extracts the unchanged current `Texture::SynchronizeHostImpl` and
`Texture::CopyToGuest` bodies. Minimal Texture, guest, memory allocation and
wait facades expose their real fields. Actual pitch-copy bodies are extracted
from `layout.cpp`; production `depth_stencil.h` and `pitch_linear.h` are included.
Unused blocklinear, BC decoder, directly mapped image, and fence branches throw
if reached, so unsupported test paths cannot pass silently.

Twenty cases cover D24S8 in both physical guest orders, D32S8, D16 and S8,
one and three array layers, and Linear/Pitch guest layouts. Every case checks
real guest bytes through `SynchronizeHostImpl` against independently assembled
Vulkan aspect planes. Independent simulated GPU aspect data is then passed to
`CopyToGuest` and checked against every expected guest byte, including unchanged
pitch-row and inter-layer padding. D32S8 padding bytes are zeroed as required by
the production packing contract. Cases use one mip; mip-plane/GPU transfer
coverage lives in `gpu_depth_stencil_vulkan`.

The executable uses AddressSanitizer and UndefinedBehaviorSanitizer with errors
fatal. Leak detection defaults off because LeakSanitizer cannot inspect the
container's `/proc` thread view; this does not disable bounds/UB checks.

Two independent baseline negative controls reproduce the original bugs:

```
python3 tests/gpu_depth_stencil_guest/run.py --ref 3065a7e3 --upload-only
python3 tests/gpu_depth_stencil_guest/run.py --ref 3065a7e3 --readback-only
```

The first must report an ASan heap-buffer-overflow from the old per-layer
`surfaceSize` copy. The second must fail guest-content equality because the old
Linear readback copied guest bytes into the host buffer. These deliberately use
S8 with one mip, isolating those copy defects from combined-aspect packing.

This harness tests the production CPU serialization bodies, not Android,
guest memory traps, full public synchronization orchestration, or Vulkan
execution. The separate Vulkan harness exercises production transfer commands.
