# Dynamic compute integration

Run `python3 tests/gpu_dynamic_integration/run.py` from the repository root.

The harness compiles exact production bodies for the pool reader, real interval
list, cached descriptor command and builder method, complete compute pipeline
creation, variant lookup, and complete compute dispatch. Controlled host
collaborators verify split mappings, content writes, remaps, incomplete/unmapped
metadata, queued/GPU writes, descriptor limits, set layout composition, pool
variants, QMD invalidation, dynamic-before-static preparation, descriptor set zero
push/update/absent cases, binding order, and texture/feedback barriers under UBSan.

A separate syntax pass compiles production cached-set recording and pipeline
creation against the pinned native Vulkan-Hpp headers. It does not execute a GPU
or replace Android/device validation.

Strict texture and sampler bodies run with the actual packed TIC/TSC types in
`gpu_texture_components` and `gpu_samplers`; descriptor snapshot ownership and
fence lifetime run in `gpu_dynamic_descriptors`.
