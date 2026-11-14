# Reserved TIC audit regressions

Run from the repository root:

```sh
python3 tests/gpu_tic_validation/run.py
python3 tests/gpu_tic_validation/run_shader.py --deps app/libraries
python3 tests/gpu_tic_validation/run_vulkan.py
python3 tests/gpu_tic_validation/run.py --probe-cache
```

Initialize the pinned shader-compiler submodule first. C++20 and Python are
required. `run.py` uses ASan/UBSan by default (`SANITIZE=0` disables them).
Leak detection is disabled for tracing hosts; memory/access UB checks remain.
`run_shader.py` uses the actual pinned TexturePass and IR implementation with
UBSan. `--deps` supplies fmt/range/Boost headers; system Boost is also supported.

`run_vulkan.py` requires spirv-as, spirv-val, Vulkan headers/loader,
VK_LAYER_KHRONOS_validation, and a device supporting nullDescriptor and cube
arrays. Mesa lavapipe is sufficient. Use VULKAN_HEADERS_INCLUDE_DIR,
VULKAN_LOADER_LIBRARY, VK_ICD_FILENAMES, VK_LAYER_PATH, and standard loader paths
for a non-system installation. A missing dependency/feature is a failure, not a
silent skip. It assembles/validates SPIR-V, dispatches seven sampled image
dimensionalities plus a storage write/read, and checks 64 output words for zero.
It verifies Vulkan behavior only, not Maxwell reserved-target behavior.

The host fixture executes the actual TIC definition, GetTextureType, the entire
GetTexture prefix before guest texture construction, actual LookupBlock methods,
and the complete sampled/storage binding functions. Segment table storage,
resource allocation and scheduling are facades. Valid texture construction is
not exercised by this fixture; gpu_sampled_layout covers its layout arithmetic.

The optional `--probe-cache` intentionally reports a **real unresolved failure**
in expected correctness. It compiles actual graphics and compute cache lookups,
including a populated transition cache and current-pipeline return. Pipeline
construction is a facade that captures the type read at construction time.
The probe must not be presented as a passing expected-failure test. The PR's APK
workflow invokes it as a mandatory gate until specialization reuse is safe.

See [the semantic audit](../../docs/pr454-semantic-audit.md) for evidence,
preexisting test failures, review and remaining limitations.
