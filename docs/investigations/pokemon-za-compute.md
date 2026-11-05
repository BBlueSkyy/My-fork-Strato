# Pokémon Legends: Z-A: compute pipeline failure capture

## Evidence and uncertainty

The supplied Moto G100 report identifies Snapdragon 870 / Adreno 650, Android
12, QualcommProprietary0.777.0, and Vulkan 1.1.276. It reports unimplemented NGC
command 1 at 30,325,743 microseconds and `vkCreateComputePipelines` returning
`ErrorUnknown` at 52,605,006 microseconds. The report has no compute shader hash,
generated SPIR-V, or descriptor layout for that failure.

These events are separated by about 22.28 seconds. Their order alone does not
establish causation. NGC dictionary protocol behavior is investigated separately
in [pokemon-za-ngc.md](pokemon-za-ngc.md). This instrumentation neither asserts an
NGC/GPU relationship nor changes the NGC service or shader output.

Unknowns include the offending compute shader and pipeline state, whether its
SPIR-V satisfies Vulkan validation, which driver feature or instruction causes
the failure, and whether corrected NGC behavior changes this later failure.
`VK_ERROR_UNKNOWN` is a result, not a diagnosis of invalid SPIR-V or a known
driver defect. There is no shader workaround in this change.

## Explicitly opt-in capture

In **Settings → Debug**, enable **Capture compute pipeline diagnostics** before
launching the game. The default is off. The same setting is available in custom
game settings; if custom settings are enabled for the game, enable it there.
The native setting is sampled when the shader manager is constructed; restarting
emulation is required after changing it.

While enabled, every newly created compute pipeline replaces a single candidate
under:

```text
shader_dumps/<title-id>/compute_diagnostics/
    guest.bin
    shader.spv
    metadata.txt
    result.txt
```

`pipeline_key` encodes the guest hash, workgroup, local/shared memory, and texture
constant-buffer slot; it distinguishes state variants of the same shader.

The guest binary is the effective Maxwell input after any guest replacement.
`guest_hash` remains the emulator's original guest shader cache key;
`guest_replacement` explicitly reports replacement use. `shader.spv` is the exact
post-replacement SPIR-V passed to `vkCreateShaderModule`, not a regenerated copy.
The complete candidate is closed to disk before `vkCreateComputePipelines` is
called. A native driver crash can therefore leave a candidate even when no Vulkan
exception/result is returned. Normal process termination does not require the
asynchronous logger to flush those payloads. This is not a power-loss durability
guarantee.

`metadata.txt` records the complete packed compute state (guest hash, workgroup
dimensions, local/shared memory, texture constant-buffer slot), guest offset,
byte lengths, capture time, entry point, descriptor set layout binding types and
counts, push-descriptor use, layout/pipeline/stage flags, absence of specialization
and push constants, physical device and driver properties, pipeline cache UUID,
all effective compiler profile fields, and the emulator's feature/quirk summaries.
The profile and traits report the effective translator configuration; they are
not a serialization of the complete Vulkan device feature `pNext` chain.

`result.txt` records the exact signed Vulkan result and symbolic name on a
returned success or `vk::SystemError`. On failure, a synchronous
`ComputePipelineFailure` log entry also contains that result, guest hash, path,
and whether the payload and result were written. The original exception is
rethrown unchanged. A missing result means the call did not return through either
of these paths; it does not prove which component crashed.

The slot contains at most two 2 MiB shader payloads, metadata below 64 KiB, and a
small result file. It does not retain earlier successful shaders. Oversized or
unavailable payloads invalidate the previous slot and write an explicit
`status=omitted_*` marker; a truncated file is never presented as complete SPIR-V.
I/O or diagnostic allocation failure does not change shader or pipeline behavior.
There are no capture file writes or SPIR-V copies when the setting is off.

Capture can slow pipeline compilation. Disable the setting after reproduction.
A later emulation session can overwrite the slot, so export before launching again.

## Reproduce and export on the reported phone

1. Record the app build/commit, game version, and selected driver. Enable capture
   in the settings that actually apply to this game. Keep other GPU settings
   unchanged for the first run.
2. Launch the same save/path that produced the failure. Reproduce once, then
   return to the game list without starting another game/session.
3. Use the existing **Share Logs** action to save `emulation.log`. Check for
   `ComputePipelineFailure` and its `capture_written=true` marker.
4. Open the Android system Files picker, select its **Strato** storage provider,
   then copy the entire `shader_dumps/<title-id>/compute_diagnostics` folder to
   the investigation machine. This uses the app's existing DocumentsProvider;
   it does not require root or access to Android's restricted data directory.
   Share Logs exports the log only, not these binary files.
5. Keep the four files together with the log. Confirm `capture_time_unix_us` and
   `guest_hash` match this reproduction. If the log reports a failed capture or
   metadata says `omitted_*`, do not interpret old/partial files as the failing
   candidate. A native driver crash may have no failure log or `result.txt`; the
   pending complete candidate is still useful, with that limitation recorded.

Inspect the exported files without extra Python dependencies:

```sh
python3 tools/validate_compute_capture.py /path/to/compute_diagnostics --inspect-only
```

To run offline SPIR-V validation, install SPIRV-Tools (`spirv-val` on PATH) and run:

```sh
python3 tools/validate_compute_capture.py /path/to/compute_diagnostics
```

The tool rejects missing/oversized payloads, byte-count mismatches, invalid SPIR-V
headers, and incomplete capture metadata before invoking `spirv-val`. It chooses
`--target-env vulkan1.1` or `--target-env vulkan1.1spv1.4` from the recorded compiler
profile, matching Strato's requested Vulkan API and SPIR-V 1.4 extension path.
Its output includes the shader SHA-256 and recorded Vulkan result. Inspection
alone is not SPIR-V validation. Validation success does not prove the driver can
compile or execute the module, or that descriptor/runtime accesses are correct.

## Host verification and remaining device work

```sh
bash tests/compute_diagnostics/run.sh
python3 tests/compute_diagnostics/syntax_vulkan.py /path/to/include
```

The first command runs the real bounded file store with ASan/UBSan and Python
capture validation tests: disabled no-op, byte preservation, replacement and
stale-result removal, payload/metadata caps, unavailable payloads, I/O failure,
valid/pending candidates, malformed/truncated input, and size mismatch rejection.
The second compiles the exact production pipeline-creation function and shader
capture/profile methods against real Vulkan-Hpp and `Shader::Profile`, with
non-Vulkan app/compiler context stubs. The include directory must contain
`vulkan/vulkan_raii.hpp`. This verifies that boundary's API/types, not the complete
Android translation units or real Vulkan execution.

A full Android build and the Moto G100 reproduction remain necessary. No supplied
failure shader was available for SPIR-V validation or a driver comparison. Useful
next evidence is the complete exported candidate and the full `spirv-val` output,
followed by a controlled comparison on the same game/save with another supported
driver. A changed driver outcome would be evidence about this pipeline, not proof
that the earlier NGC event caused it.
