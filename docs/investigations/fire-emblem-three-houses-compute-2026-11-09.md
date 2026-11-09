# Fire Emblem: Three Houses — second compute-pipeline failure (2026-11-09)

## Evidence from the new user device log
- Same game: Fire Emblem: Three Houses (`010055D009F78000`), v1.0.0, without update.
- Moto G100 / Adreno 650, Qualcomm proprietary Vulkan driver `0.777.0`.
- The PR #434 quirk `needsIndividualTexelBufferBindingWrites` is **true**, and the original early `vkUpdateDescriptorSets` crash was not observed during this longer session.
- The later failure is: `ERROR | 160013359 | GPFIFO | Run: vkCreateComputePipelines: ErrorUnknown`.
- The old crash was in `Sky-CmdRecord / vkUpdateDescriptorSets`. Do not assume the two failures share a cause.
- The log **does not identify the shader hash, emitted SPIR-V or Validation Layer VUID**. `VK_ERROR_UNKNOWN` alone is insufficient to conclude whether the driver, shader translation or a pipeline parameter is defective.

## Diagnostic only
From the confirmed HEAD of PR #434, add Debug-only `FE3H-COMPUTE SHADER` and `PIPELINE_BEGIN/PIPELINE_END` markers within `kepler_compute/pipeline_manager.cpp`. Every shader log includes the guest shader hash, binary base offset/size, workgroup dimensions, local/shared memory and bindless texture constant-buffer slot. Every pipeline creation log includes the hash, workgroup, memory and descriptor-set layout-binding count.

Preserve the complete PR #434 texel-buffer fix. Do not rewrite guest shaders, manipulate compute pipeline parameters, suppress errors or change graphics descriptor paths. Compile only ARM64 DevDebug with pinned Vulkan Validation Layer 1.4.363.1 and synchronization validation, using the existing known-good workflow from PR #434's earlier diagnostic build.

## Required test
Reproduce the **later** crash using Qualcomm proprietary driver. Send full `emulation.log`, including `FE3H-COMPUTE PIPELINE_BEGIN` and last `PIPELINE_END` around `vkCreateComputePipelines`, any Validation Layer VUIDs and driver details. If reproducible and resources permit, repeat the same scene using Turnip (record its precise driver version).

After the failed shader hash is known, consider capturing *only its SPIR-V* for offline `spirv-val` and targeted shader inspection. No fixes should be inferred from `VK_ERROR_UNKNOWN` alone.

Keep the PR Draft and **do not merge**.

## Captured culprit — targeted shader `0x45E67C25A776FB35`

The latest DevDebug `emulation.log` (374 lines) shows:
- `FE3H-COMPUTE SHADER hash=0x45E67C25A776FB35 baseOffset=0x27C200 guestBytes=6224 workgroup=64,1,1 localMemory=0 sharedMemory=512 texCbuf=2`.
- `FE3H-COMPUTE PIPELINE_BEGIN` with 6 bindings, no `PIPELINE_END`, then `vkCreateComputePipelines: ErrorUnknown` in GPFIFO.
- Other compute hashes `0x626A2EB8EB9D0A2B` and `0x05CE7D2199E0BB8D` created pipelines successfully.
- The Validation Layer reported texel alignment, image dimension and numeric-format VUIDs earlier, but no specific VUID at this pipeline creation. Those earlier errors should not be assumed causal.

### Next diagnostic build

Only for this hash and **only in DevDebug**, `ShaderManager::ProcessShaderBinary` saves the guest Maxwell binary as `45E67C25A776FB35.maxwell` and generated SPIR-V as `45E67C25A776FB35.spv` in the existing `shader_dumps/<title id>` path. The `FE3H-SHADER FILE` log prints the exact paths. It also prints the full SPIR-V as ordered `FE3H-SPIRV BEGIN/CHUNK/END` hex chunks (up to 256 KiB), so the user can provide **only emulation.log** for reassembly. The compute pipeline manager also emits the 6 layout binding types/counts.

After reconstructing the output, validate it with `spirv-val`, inspect capabilities/extensions and the six binding declarations, and compare with the Qualcomm proprietary Vulkan 1.1.276 feature set. If SPIR-V is valid, additional runtime A/B may be needed to isolate a driver incompatibility. Do not implement a speculative shader/compiler fix and do not merge.
