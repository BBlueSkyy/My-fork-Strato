# Production GMMU identity integration

`ASAN_OPTIONS=detect_leaks=0 python3 tests/gpu_gmmu_identity/run.py`

The runner extracts unchanged production map/unmap, changed-range comparison,
fast/fragmented translation and tracked capture bodies. Only block/segment table
storage is a host facade. Tests include no-op subranges, partial substitution,
unchanged-prefix/suffix generations, IOVA P→Q→P, gaps, sparse removal and repeated
unmap. The final unmapped tail is checked with trimmed table capacity so ASan
reliably detects an end-iterator overread; a full-range replacement over mixed
blocks preserves the unchanged endpoint generations. Real Vulkan content/cache/lifetime tests run in gpu_backing_promotion with
`TEXMAN_MAPPING_PROBE=1` and the normal authority probe.

No required CI job. This proves translation identity, not physical allocation
reuse or partial sparse image authority.
