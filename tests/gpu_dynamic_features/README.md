# Dynamic texture-handle feature gates

Run `python3 tests/gpu_dynamic_features/run.py`. When the shader compiler is
checked out separately, set `SHADER_COMPILER_ROOT` to that checkout. The
`VULKAN_HPP_INCLUDE` and `VULKAN_HEADERS_INCLUDE` overrides are also supported.

This test compiles the production TraitManager constructor, feature-chain
alias, GPU feature query and ShaderManager profile initializer against the
pinned Vulkan headers. Physical-device queries and driver quirks use the
existing host fixture. It also compiles the actual compute and graphics gates,
checking each graphics stage against its enabled storage/atomic feature.
Optional shaderResourceMinLod enablement and profile propagation are checked.

The 288 cases vary application/physical-device Vulkan versions, extension
availability and every combination of the four required feature bits. They
check atomic feature enablement, unsupported pNext unlinking, profile and
compute gates, preservation of actual descriptor/TIC limits and absence of
unused update-after-bind/variable-count features. In particular, a Vulkan 1.3
physical device does not supply core descriptor indexing to a Vulkan 1.1
application without `VK_EXT_descriptor_indexing`.

The existing `tests/gpu_shader_features/run.py` remains the storage8/storage16
and arithmetic-feature regression suite.
