# TIC numeric component queries

Run `python3 tests/gpu_texture_components/run.py`. Set `SHADER_COMPILER_ROOT`
to the shader compiler checkout and `BOOST_INCLUDE_DIR` to the Boost headers
when they are not available at the runner's default paths.

The UBSan host test extracts the exact production TIC format conversion,
numeric classifier, texture query, pool flush and generation query, and the
initial view-cache lookup block. The TIC descriptor, format metadata, aspect
selector and shader numeric enums also come from production. Only initializer
spelling/order in the metadata table is adapted for GCC; values are preserved.

The strict dynamic texture helpers and requested-mapping recording block also
compile and execute against the actual TIC and native Vulkan types. Cases cover
unsupported modes, mip ranges, device limits, format features, image usage,
tail-page remaps, same-TIC aliases, retained queued views, snapshot numeric types,
and restoration of the live pool span on success and exception.

Coverage includes every supported UINT/SINT TIC family, float and sRGB,
depth/stencil aspect selection, invalid/out-of-range entries, same-size pool
rebinding within one channel sequence, and monotonic pool flush generations.
The actual graphics and compute environment numeric callbacks and runtime/file
accessor bodies are compiled and exercised for signedness preservation.

The complete production pipeline bundle class and serialization/deserialization
bodies verify Float/Uint/Sint round-trips and reset behavior; byte spans and the
checksum function are host facades. This tests layout and value persistence,
not the checksum algorithm. The runner also requires cache format version 6.
This is a host behavior test, not mobile Vulkan validation or game acceptance.

The following source-mutation negative controls must fail at their named
behavioral assertion (they are targeted regressions, not historical baselines):

| `TEXTURE_COMPONENT_NEGATIVE_CONTROL` | Expected failure |
| --- | --- |
| `collapse-sint` | integer TIC family lost numeric signedness |
| `ignore-stencil` | stencil view must use unsigned numeric type |
| `ignore-pool-generation` | same-sequence pool rebind reused stale view |
| `collapse-bundle` | bundle lost numeric component type |
