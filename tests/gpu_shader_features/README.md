# Vulkan shader feature negotiation regressions

`python3 tests/gpu_shader_features/run.py` compiles the real TraitManager
constructor, its feature-chain declaration, the actual CreateDevice query
list and the ShaderManager profile initializer against Vulkan-Hpp. Only
physical-device queries and driver quirks are host facades.

The 64 combinations vary arithmetic int8/int16, storage8/storage16, the
8-bit-storage extension and typeless image writes. This checks support vs
enabled state, extension gating, pNext unlinking and independent profile
fields. Strato requests Vulkan 1.1 even if a device reports a newer API.

For the negative control, set `TRAITS_BASELINE_REF=fix/scheduler-yield-progress`.
The old constructor fails the enabled-state assertions against the current
feature-chain declarations. `SHADER_COMPILER_ROOT`, `VULKAN_HPP_INCLUDE` and
`VULKAN_HEADERS_INCLUDE` can point to host dependency checkouts.

The same constructor exposes an effective SSBO alignment of at least four
bytes and a word-sized maximum range to both compiler and binder. Tests
exercise physical alignments 1 through 256 and 3,840 odd-address/short-range
combinations using the exact production binding range calculation.
