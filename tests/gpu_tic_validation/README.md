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

`--probe-cache` now exercises correct specialized reuse rather than accepting
stale metadata. It compiles actual graphics/compute lookups, populated transition
cache, current-pipeline return, clean-register Refresh, v5 preload reconstruction,
compile-CBUF safety and the exact descriptor-update lambda from PrepareDraw.
It verifies restored variant reuse, unchanged construction counts, current-source handle
matching, integer/sampler changes, forced full descriptors, retry, unbind and
short-view handling. Pipeline construction remains a dependency-capturing facade.

Add `--vulkan-lifetime` to instantiate real VkPipelines in that same manager
fixture. A timeline semaphore holds an old submission/fence pending across ten
variant creations; all remain owned, then the semaphore is signaled and the
fence completes with no validation errors. This requires Vulkan1.2 timeline
semaphores, validation, spirv-as and spirv-val. Use the same Vulkan environment
variables as run_vulkan.py. The compute shader is a no-op: this checks resource
lifetime, not Kirby rendering. Shader/predicate replay is in `replay/` and accepts
the user-supplied PCHE staging cache without publishing game binaries.

The original four-failure output is preserved in audit-results.txt. LookupNext's
correct miss is nullptr; managers construct/select variants. Tests assert that
contract, not dereference a deliberately rejected stale pointer. A failed
correctness assertion still fails CI and blocks APK assembly.

See [the semantic audit](../../docs/pr454-semantic-audit.md) for evidence,
preexisting test failures, review and remaining limitations.

Continuation and current limits: [PR456 specialization fix](../../docs/pr456-specialization-fix.md).

The [PR471 audit](../../docs/pr471-semantic-audit.md) adds split-handle CBUF
regressions: absent sources independently contribute zero, bound ranges are
checked before reads, and pending GPU data unwinds descriptor preparation.
The production full graphics, quick-bind and compute handle snapshot passes
execute with ASan/UBSan, including stage 4 / slot 16, arrays and cross-stage
ordering. Output-binding dirtiness is simulated; these are host tests.
`run_shader.py` also runs the pinned TexturePass on a synthetic split c[2]/c[16]
handle with the secondary absent/bound. It does not replay Sniper Elite 3 shaders
or establish unbound-bank behavior on Maxwell hardware.
