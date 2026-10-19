# Dynamic compute descriptor tables

Run `python3 tests/gpu_dynamic_descriptors/run.py`.

The UBSan executable exercises production capacity, compact index ABI, changed
range calculation, and snapshot reuse/clone/recycle decisions. A second UBSan executable runs the complete production runtime with a validating
descriptor device and deterministic fence fixture. It verifies unchanged dispatch
set reuse, delta-only writes, initialized descriptor copies, invalid sampler
holes, backing changes, overlapping executions, completed-set recycling, fault
reporting/reset, resource preparation, no repeated blocking layout transition for
unchanged candidates, and lifetime after pipeline/sampler-owner destruction.

The native smoke
compiles the complete production runtime against the repository's pinned
Vulkan-Hpp, replacing only Android/engine interfaces with declarations. It checks
real Vulkan descriptor layouts, pools, allocation, copies, updates and handles.

These checks do not execute a GPU shader. Runtime SPIR-V tests must additionally
verify nonuniform SSBO-selected handles, numeric variants, invalid-index fault
reporting, and descriptor/resource lifetime across multiple submissions.
