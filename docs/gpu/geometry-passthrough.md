# Geometry shader passthrough integration

Baseline: Strato master `3065a7e3a85633d6b3faffd4bcefa6c171b5cbbb`.
Compiler baseline: the gitlink already used by that master, `dcb938866f4234963fcbc199f2443438d8ef6f07`.
Compiler integration: `7657b7d17f0163e90d1c310773655bb9629280da` (published on `fix/geometry-passthrough-support`).

## Proven defects and changes

- Lowering generated ordinary GS instructions but retained the native passthrough flag. PipelineManager consequently applied host depth conversion in both VS and lowered GS. Synthetic Layer GS was also excluded from its physical-stage decision. Both now have a single owner for final position conversion and transform feedback.
- Whole-vector copies overwrote skipped components. Lowering now copies the exact requested components, including Position, generic varyings, PointSize and ClipDistance, and supports primitive ID forwarding.
- Vulkan output values become undefined after EmitVertex, whereas native passthrough outputs are per primitive. The SPIR-V fallback preserves guest-computed outputs across emissions. This work is confined to compilation and the affected shaders.
- Native emission is distinct from guest provenance. It requires the negotiated NV extension, valid masks and a safe interface. Mixed masks, legacy varyings, guest position access (including indexed access), multiple invocations and primitive builtin inputs use lowering. Strato also uses lowering for adjacency, transform feedback and tessellation combinations.
- Layer and ViewportIndex can be carried from the predecessor through collision-free generic inputs, including existing LayerPass aliases. The synthetic GS handles both builtins and line topology. Generated passthrough loads select the central adjacency vertices.
- Native decorations now require an actually native program, including the Position block workaround. Unsupported native emission fails compilation rather than emitting an invalid module after a warning.
- No new per-draw allocations, locks, submissions, fence waits, queue/device idle calls, worker threads or polling were added. Existing pipeline caches are retained; the serialized cache format is unchanged.

## Device loss is a separate, unresolved observation

The supplied NieR log has approximately 11 seconds between the passthrough warning and loss observed on Sky-Present. It contains no fault report or offending shader/command identification. These compiler defects do not establish the cause of that device loss.

VK_EXT_device_fault is queried and enabled only when advertised and its deviceFault feature is supported. Vendor binary generation is disabled. A bounded, once-per-device report is collected only after an actual typed device-loss error on the covered submit, fence waiter or presentation paths. Reporting cannot replace the original exception. The existing five-second submit-error sleep was removed. Pinned Vulkan-Hpp predates the extension, so guarded raw Vulkan ABI declarations avoid a dependency upgrade.

Feature negotiation and error paths were tested with synthetic API callbacks, not an actual Qualcomm fault. Driver fault details remain unavailable until another device failure is observed with this APK.

## Replacement E08654FB1F946CD5

This is an external replacement loaded from `shader_replacements/<SaveDataOwnerId>` in the app's public files directory. A `.spv` file supplies host SPIR-V; other extensions supply guest Maxwell code. No matching replacement file exists in the repository or supplied log. The existing replacement log now identifies which type was used.

Original guest binaries remain in pipeline bundles; original host SPIR-V is generated before replacement. No replacement was modified or deleted. A real A/B test has not been performed: preserve the external file, move it outside the scanned directory, restart the process, and compare equivalent cold-cache runs. Merely renaming the extension to `.bak` inside the directory still loads it as guest code. Use separate cache-directory copies for the two arms, as replacements may request different descriptors or constant-buffer dependencies.

## Executed validation

- Complete host compiler build (all existing compiler backends), regression fixtures and independent integration review cases.
- `spirv-val --target-env vulkan1.1` accepted 17 production-emitted modules: native, lowered, partial masks on native-capable profiles, Position block workaround, PointSize input, primitive-output lifetime, synthetic GS and adjacency.
- Production emitted modules were run on llvmpipe with Vulkan validation enabled. Direct VS/FS and lowered/synthetic GS had matching coverage and per-vertex varying values: points 1 sample; lines 24 samples; triangles 288 samples. Repeated draws and synthetic adjacency fixtures also matched. No validation errors occurred. These are small offscreen fixtures, not game frames.
- Runtime policy tests passed under UBSan. Device-fault collector tests passed under ASan/UBSan, including optional-feature gates, one-shot concurrency, incomplete reports, bounded output and failures.
- Actual device creation/reporting methods and feature negotiation compiled against Vulkan-Hpp/headers 202 and 275. Fence loss preserves its typed Vulkan error.
- Existing gpu_queries, gpu_conditional and gpu_draw_texture regressions passed.

Reproduction:

```sh
bash app/libraries/shader-compiler/tests/run.sh
python3 tests/gpu_geometry_pipeline/run.py
python3 tests/gpu_device_fault/run.py
python3 tests/gpu_device_fault/compile_vulkan.py
python3 tests/gpu_geometry_pipeline/run_vulkan.py --require-synthetic app/libraries/shader-compiler/tests/build/modules
```

The offscreen test requires a Vulkan graphics ICD, validation layers, glslangValidator and SPIR-V tools. The new Geometry Passthrough Regression workflow installs these and uses lavapipe. The existing PR Build workflow produces Android ARM64 dev release/reldebug APKs.

## Compiler performance comparison

An unchanged `tests/benchmark.cpp` was built against the baseline gitlink and the corrected compiler, both Release with GCC 13.3, the same fmt/Sirit/range/Boost dependencies, process CPU time and logging I/O disabled. Each case runs 7 samples of 20,000 fresh translations plus SPIR-V emissions; pools are reused as in ShaderManager. Alternating baseline/corrected runs were pinned to the same available CPU. Values below are the medians from two runs, in microseconds per compile.

| Fixture | Master medians (us) | Corrected medians (us) | SPIR-V words, master -> corrected |
| --- | --- | --- | --- |
| Ordinary vertex | 7.382 / 6.889 | 9.475 / 6.577 | 231 -> 231, byte-identical |
| Ordinary geometry | 7.025 / 6.735 | 7.288 / 6.371 | 220 -> 220, byte-identical |
| Full passthrough | 23.187 / 27.978 | 23.317 / 22.746 | 726 -> 726 |
| Partial passthrough | 22.865 / 22.978 | 18.880 / 18.597 | 726 -> 564 |

An earlier run showed variable increases; these were investigated by removing log I/O from the harness, using process CPU time, alternating revisions, and avoiding empty preserve-mask and skipped-attribute scans in production. The final ordinary-shader timings overlap and vary substantially, so they cannot establish a small regression or improvement. Partial-mask fixtures consistently compile faster and generate fewer words because skipped components are no longer copied. These fixtures are not representative measurements of NieR compilation or gameplay. No extra physical pipeline was introduced for passthrough: the fallback and synthetic GS already existed. Distinct game pipeline counts were not measured.

To reproduce, build `geometry_benchmark` from the compiler test CMake project; run `geometry_benchmark 20000` against each revision using identical dependencies and CPU affinity. Warm the device and alternate revisions. Record raw samples rather than selecting a single run.

## Still requires Android/device testing

No NieR:Automata executable, game shader capture, external replacement binary or connected Android GPU was available. Consequently, this change does not claim NieR boots or renders correctly, does not attribute its device loss to these defects, and does not guarantee game FPS/frametime or driver compilation time. Native NV passthrough was validated structurally; a native-capable GPU was unavailable. Turnip and Qualcomm proprietary execution remain untested here.

On the same device, compare master and this APK with identical driver, settings, game/update, scene, battery/thermal conditions and separately preserved cold/warm caches. Record at least three comparable runs of FPS, median/p95/p99 frametime, shader compilation time and pipeline counts. Include games that already render correctly and the replacement A/B when the file is available. A reproducible regression must be resolved before merging.

Indexed predecessor writes of primitive builtins and exhausted varying slots fail explicitly; silent corruption is not substituted for unsupported interfaces.

## References inspected

- Khronos Vulkan geometry passthrough semantics: https://docs.vulkan.org/spec/latest/chapters/geometry.html
- NV Vulkan extension requirements: https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_geometry_shader_passthrough.html
- Vulkan builtin interface rules: https://docs.vulkan.org/spec/latest/chapters/interfaces.html
- Local Eden `14da4482a4668928bad43bb67ef12f96b9c9d972` retained the original whole-vector lowering; it was not treated as proof of correctness.
- Local Ryujinx `534f92506bf8c6aba29bb86917bf10954a360d8e` derives passthrough vertex counts from topology and chooses central adjacency vertices.

The branch does not include Texman V2, NCA, HID or guest scheduler changes. No merge is authorized or performed.
