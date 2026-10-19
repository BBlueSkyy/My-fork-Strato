# Sampler pool cache host regression

Run `python3 tests/gpu_samplers/run.py` from the repository root. Requires
Python 3 and g++ with C++20 and UBSan support.

The runner compiles the production dirty-state manager, TSC layout and sampler
pool/cache implementation. GMMU mappings and Vulkan object construction are host
interfaces. GCC requires a spelling-only conversion of Clang's nested designated
border-color initializer; sampler conversion and cache behavior are unchanged.

The regression rebinds an equally sized sampler pool containing a different
filter, through the real register dirty manager. Before the fix it fails with
`same-size sampler pool rebind reused stale index cache`. It also checks sampler
reuse, the old object's retained lifetime, linked versus explicit index selection,
and that an invalid index remains absent rather than receiving a replacement.

The full dynamic sampler helpers and pool reader exercise strict filter/address
validation, normalized coordinates, comparison, LOD and anisotropy limits,
reduction/extension gates, linear capability, shared sampler ownership, and
restoration of live static TSC reads after a dynamic snapshot lookup.

This does not validate Android driver behavior, prove the source of MK11's
out-of-range handles, or make null Vulkan sampler descriptors valid. No game
files are required.
