# TIC mapping regressions

Run `python3 tests/gpu_texture_mappings/run.py`. Set `CXX` or
`BOOST_INCLUDE_DIR` if the host compiler/Boost headers are outside standard paths.

The host harness extracts complete production texture pool, image lookup,
metadata and texel-buffer lookup methods. It compiles the real TIC layout,
GMMU translation and dirty tracking with ASan/UBSan and checked STL spans.
Vulkan allocation and buffer-view creation are host interfaces; image mappings
are inspected after the actual production cache lookup and translation.

Coverage includes unchanged TIC bytes across resident/sparse/unmapped remaps,
same-generation address-space changes, both TIC cache layers, ordinary submit
reuse, disjoint descriptor tables, unused unmapped pool tails, split descriptors,
sparse descriptor zero reads and direct aliases that observe descriptor writes.

Optional `cache` or `pool` arguments select a focused group. For demonstrating
the original cache failure, set `TEXTURE_MAPPINGS_BASELINE_REF=53e1c10b` and run
with `cache` (the baseline replaces only the production `GetTexture` method).
