# Geometry pipeline regressions

`python3 tests/gpu_geometry_pipeline/run.py` compiles the production runtime
policy and synthetic output topology converter with small host state interfaces.
It uses the real compiler `RuntimeInfo`, varying masks, and attribute enums, and
runs with UBSan. Cases cover final-stage depth conversion and transform feedback,
native passthrough point size, synthetic GS presence, line topologies, and legacy
varying allocation between stages.

`python3 tests/gpu_geometry_pipeline/run_vulkan.py <compiler-build/modules>`
runs the shader compiler's emitted `points-lowered.spv`, `lines-lowered.spv`, and
`triangles-lowered.spv` through real Vulkan graphics pipelines. It also checks
`points-synthetic.spv`, `lines-synthetic.spv`, and `triangles-synthetic.spv` when
present. Synthetic `lines-adjacency.spv` and `triangles-adjacency.spv` select the
central vertices from deliberately different surrounding adjacency vertices; their
coverage is compared to the matching direct line/triangle. Use
`--require-synthetic` to require all five synthetic fixtures. It requires a
Vulkan ICD with geometry shaders and precise occlusion queries, the Khronos
validation layer, Vulkan headers/loader, `glslangValidator`, and `spirv-val`.
Mesa lavapipe supports headless execution. Standard environment include/library
paths, `VK_ICD_FILENAMES`, and `VK_LAYER_PATH` can select portable dependencies.

The Vulkan test compares direct VS/FS coverage against the lowered GS, checks
per-vertex location 0 varying values against interpolated screen position by
discarding mismatches in the FS, repeats the draw,
and fails on validation errors. It does not require game files. These simple
fixtures do not establish correctness for tessellation, arbitrary adjacency workloads, transform
feedback, all built-ins, or a particular mobile Vulkan driver.
