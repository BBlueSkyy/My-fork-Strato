# GMMU residence proof for regional textures

Run `python3 tests/gpu_region_residency/run.py` (ASan/UBSan; no APK CI job).
The exact production fast/fragmented translation and block-lookup bodies run
against a small segment/block-table facade. A non-null sparse placeholder is
not accepted as resident; ordinary fragmented resident mappings retain their
previous merging behavior. Repeated translation reflects changed residence.

This tests a translation snapshot, not pending GPU remap lifetimes, generation
identity or partial sparse texture transfers. The Vulkan backing suite checks
that the false residence proof rejects texture admission before allocation.
