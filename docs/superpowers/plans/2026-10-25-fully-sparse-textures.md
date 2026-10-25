# Fully sparse sampled textures

Goal: selectively adapt #373 onto master 9935f133, preserving resident paths.
Spec: the user's 2026-10-25 request (Oceanhorn 2, 2048x2048/512x2048, 12 mips).

1. Reproduce the split-range sparse-pointer bug with production code under ASan/UBSan; port its fix. Add a locked whole-range sparse query that rejects holes and physical pages. Test adjacent sparse regions, mixed mappings and a later physical remap.
2. Port only the sampled 2D zero-image cache. Validate dimensions/mips/views, compute the RGBA8 mip chain with checked arithmetic, and cap image memory requirements to the remaining 32 MiB budget and Vulkan limits. Clear all levels with an explicit undefined -> transfer -> shader-read dependency. Keep fresh GMMU translation ahead of sparse cache lookup.
3. Verify focused production-code regressions, existing sampled-layout tests, and Vulkan code compilation where dependencies permit. Document unsupported cases and the independent termination code. Preserve CI unchanged; publish one Draft PR, which triggers ARM64 DevRelease. Do not monitor Actions.

Review focus: sparse plus unmapped ranges; partial residency; invalid mip/view counts; device padding beyond logical RGBA8 size; physical remap after a zero-image cache hit. No changes to original #373, guest format interpretation, or PRs #393-396.
