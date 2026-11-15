# Captured Kirby fragment shader replay

The unchanged repository-pinned compiler preserves P0 in both actual user-captured
Kirby fragment shaders. In the generated offline SPIR-V, the sample associated
with `CBUF2[0x30]` is reachable only through the true edge of a comparison of
`0.0` against the live uniform `CBUF3[0x4]`. Zero skips that sample; nonzero and
NaN can reach it. All six original texture samples remain in each module.

This establishes compiler control flow for these captured inputs and the explicit
offline profile below. It is **not a dump of the SPIR-V used by the user's
Turnip device**, a GPU execution trace, or proof that an invalid TIC is safe to
sample.

## Inputs and provenance

- Strato source base: `d951ca35`; the replay does not depend on the active
  pipeline-cache specialization fixes.
- Shader compiler: `e972cae1d8de3c437769bb2b34fd23af7e030c40`, clean worktree.
- `fmt`: `dcd282bb268a0766f6d273b6e41a3a96719bbbfb`.
- `range`: `83783f578e0e6666d68a3bf17b0038a80e62530e`.
- `sirit`: `d7ad93a88864bda94e282e95028f90b5784e4d20`.
- Those dependency checkouts match this repository's gitlinks and are clean.
- PCHE v5 staging SHA-256:
  `11868f75aea624a8294907d64d0566f19b6ad85422e32922bf28ed66f856fb5b`.
- All 1270 staging bundles pass the stored XXH64 checksum, including entries
  38 and 39. The runner uses the fragment hash at its exact pipeline-key slot,
  not an arbitrary byte-string search.
- The runner takes a user-provided cache. Proprietary binaries/cache contents
  are not included in the repository.

| Fragment hash | Entry | Base address | Maxwell bytes | Maxwell SHA-256 |
|---|---:|---|---:|---|
| `E7582A61F9097487` | 38 | `0x1AE3630` | 376 | `7111bd9d587d46f251094c1e8db40f0a4c2c8ea28851ec83586bdc8c0cc8a4ff` |
| `BC3F7489079E8ABE` | 39 | `0x1AE4130` | 352 | `448e2d04ba98a7d370ced0d3c02d341759443861d9b211074b4ace37d899a2dd` |

## What is reproduced

`replay/replay.cpp` follows `ShaderManager::ParseGraphicsShader` and
`GraphicsEnvironment`: copies the captured 0x50-byte program header into `sph`,
sets Fragment stage, retains the captured guest base as `start_address`, starts
CFG construction at **base + 0x50**, uses proprietary-driver rules for CBUF2,
and uses header local-memory sizes. The initial scheduling word is handled by
the real Maxwell frontend; it is not treated as a shader instruction.

The complete production translation pipeline runs, including `TexturePass`,
then the production SPIR-V backend. There is no synthetic IR replacing the
shader, texture removal, live-uniform folding, or hardcoded P0 value. Unexpected
instruction, compile-time CBUF, or metadata callbacks throw. In particular,
`CBUF3[4]` is absent from the recorded compile-time CBUF reads, remains an
`OpLoad` from a Uniform variable, and is not supplied as a compile-time constant.

The PCHE packed graphics key has size 1496. `replay/layout.py` extracts the actual
production field declarations, substitutes ABI-equivalent fixed-size arrays,
primitive enum types, and packed 4-byte `VertexAttribute`/`CtSelect` types, then
checks Clang's `aarch64-linux-android` record layout. It verifies the full size
and the relevant offsets: flags52, alpha reference64, post-VTG mask212. Both
captured keys give:

| State | Value |
|---|---|
| Bindless texture CBUF | 2 |
| Viewport transform | enabled |
| API-mandated early Z | disabled |
| Flip Y | enabled |
| Alpha test | disabled (stored function Always; reference 0) |
| Post-VTG skip mask | eight zero words |

Fragment compile-time CBUF2 reads are replayed from stage4 records, including
`0x20=0x1040024D`, `0x28=0x10400276`, `0x30=0`. Texture-type callbacks convert
raw bindless handles to their low20-bit TIC index exactly as the renderer does.
The captured records report indices589,630,0 as `Color2D` (enum2).
**Replaying that recorded fallback metadata does not make TIC0 valid.** The log
shows TIC0 type15, all eight words `0x7FBFFFFF`, and an unmapped image address.
The comparison/integer-format callback tables are empty, and these shaders do
not request those callbacks.

## Explicit offline profile and limits

The capture does not serialize the full host `Profile`, `HostTranslateInfo`, or
the preceding translated vertex-stage output interface. The replay declares:

- SPIR-V1.4; unified descriptor binding; descriptor aliasing enabled;
  derivative control enabled; remaining `Profile` fields retain declared
  defaults, including `ignore_nan_fp_comparisons=false`.
- `HostTranslateInfo`: SNORM render-buffer support enabled, SSBO alignment16;
  remaining fields retain declared defaults.
- Resolution scaling disabled, debug/loop-safety settings use release values.
- Captured fragment runtime flags above; all preceding-stage attribute stores
  enabled for the offline interface. This is an explicit interface assumption,
  not an inferred reconstruction of the preceding device shader.

These shaders have no fp16, integer64, storage-buffer, subgroup, or shadow-sample
operation requiring the corresponding unavailable device settings. The selected
profile emits the ordinary unordered comparison and structured branches shown
below. Different device profile choices can change module bytes and interfaces;
this replay does not establish byte identity with the actual device module.
The log identifies Turnip/Adreno650, Vulkan1.3.280, Mesa24.0.99, but it does not
supply a complete serialized shader profile or device SPIR-V dump.

## Generated result and control-flow proof

| Hash | SPIR-V bytes | SPIR-V SHA-256 | P0 / sample / containing block |
|---|---:|---|---|
| `E7582A61F9097487` | 4216 | `6d8b9888ccc0694bf3ebacae097c32cbd495699c167b7ff56b0c6104aec12dda` | `%71` / `%107` / `%53` |
| `BC3F7489079E8ABE` | 4076 | `5279dfabd3da6d9fb30a598f260ec303585681ee7d32ca6eb16ea631c324311d` | `%70` / `%111` / `%54` |

For E758, the disassembly contains:

```text
OpName %37 "c3_f_4"
OpName %43 "fs_tex2_30"
%69 = OpAccessChain %36 %37 %23 %68  ; member0, float element1 => byte4
%70 = OpLoad %3 %69
%71 = OpFUnordNotEqual %2 %24 %70  ; %24 is float0
...
%52 = OpLabel
OpSelectionMerge %54 None
OpBranchConditional %71 %53 %54
%53 = OpLabel
...
%106 = OpLoad %39 %43
%107 = OpImageSampleImplicitLod %6 %106 %105
OpBranch %54
```

The BC3F module has the same relationship with predicate `%70`, true block`%54`,
false/merge block`%55`, texture`%42`, and sample`%111`.

`replay/prove.py` reads the generated disassembly and checks the actual SPIR-V
uniform pointer/array stride and byte offset, unordered comparison, texture
load/sample, branch edges, unique predecessor of the sample block, and CFG
reachability. It fails on an additional unmodeled branch predicate. It does not
rely solely on Maxwell instruction annotations or IR dumps.

| Runtime float at CBUF3[4] | P0 | CBUF2[0x30] sample block reachable | Reachable samples |
|---|---|---|---:|
| +0 or -0 | false | no | 3 of 6 |
| +1 or -1 | true | yes | 6 of 6 |
| NaN | true | yes | 6 of 6 |

These are CFG reachability checks under a specified uniform input, not GPU
execution of sample results. Both selected paths reach `OpReturn` and remain in the **same** compiled module.
The sole incoming edge to the target sample block is the true edge of P0;
there is no unconditional path into that block.

The original log's `[KIRBY-P0]` records at lines226/232 (sequences205/207)
associate the respective shader hashes with bound, CPU-readable
`CBUF3[4]=0x00000000`. Their own note says `cpu-visible-not-shader-execution`.
Combined with this replay, those observed zero values explain why descriptor
preparation can encounter the invalid TIC without the predicated sample being
executed. They do not prove the uniform value observed by every GPU invocation,
exclude synchronization defects, or authorize replacing a genuinely executed
sample with an arbitrary result.

## Reproduce

From the repository root with initialized `fmt`, `range`, `sirit` (including
SPIRV-Headers), Boost headers, Clang19, and SPIRV-Tools:

```bash
python3 tests/gpu_tic_validation/replay/layout.py \
  --clang "$CXX" --out "$REPLAY_OUT"
python3 tests/gpu_tic_validation/replay/build.py \
  --deps "$HOST_LIBRARY_DEPS" --boost "$BOOST_INCLUDE" \
  --build "$REPLAY_BUILD" --jobs 2
python3 tests/gpu_tic_validation/replay/run.py \
  --cache "$USER_STAGING_CACHE" --binary "$REPLAY_BUILD/replay" \
  --out "$REPLAY_OUT" --spirv-tools "$SPIRV_TOOLS_BIN"
```

Actual local execution used these paths:

```text
CXX=/workspace/scratch/c3a7d9bb6c03/host-deps/root/usr/bin/clang++-19
LD_LIBRARY_PATH=/workspace/scratch/c3a7d9bb6c03/host-deps/root/usr/lib/x86_64-linux-gnu
HOST_LIBRARY_DEPS=/workspace/scratch/5107f4a3d438/strato/app/libraries
BOOST_INCLUDE=/workspace/scratch/c3a7d9bb6c03/host-deps/root/usr/include
REPLAY_BUILD=/workspace/scratch/33440eae037b/kirby-replay-build
REPLAY_OUT=/workspace/scratch/33440eae037b/kirby-replay-results
SPIRV_TOOLS_BIN=/workspace/scratch/5107f4a3d438/host-tools/root/usr/bin
USER_STAGING_CACHE=/workspace/scratch/33440eae037b/kirby-evidence/KIRBY_STRATO_EVIDENCIAS/01_CACHE_PCACHE_ORIGINAL/01004D300C5AE000.staging
```

The fresh build compiled all159 required C++ translation units from the pinned
frontend, IR passes, SPIR-V backend and matching Sirit. No old compiler objects
were linked. Build manifests record source/header hashes, exact compile
commands, compiler version, dependency commits/status and binary SHA-256;
subsequent invocations can reuse only matching objects. The build uses at most
three workers (two in the recorded run).

The runner emits `.maxwell.bin`, `.env`, `.ir`, `.descriptors`, `.spv`,
`.spvasm`, compiler/validator logs and `replay-summary.json`, including the
structural proof for each shader. These are outputs in the supplied output directory,
not repository fixtures. To rerun only the proof:

```bash
python3 tests/gpu_tic_validation/replay/prove.py \
  "$REPLAY_OUT/E7582A61F9097487.spvasm" \
  "$REPLAY_OUT/BC3F7489079E8ABE.spvasm" \
  --out "$REPLAY_OUT/predication-proof.json"
```

Both modules pass `spirv-val --target-env vulkan1.1spv1.4
--uniform-buffer-standard-layout`. The feature flag is necessary because the
production backend emits uniform scalar arrays with stride4. Validation without
that feature rejects the uniform layout; Strato's trait manager explicitly
requires/assumes this layout capability. It is not suppressed validation of an
unrelated error. `spirv-dis --raw-id` produced the modules used in the proof.

Additional checks passed: Python syntax compilation; rejection of a checksum
corrupted PCHE bundle; rejection of a disassembly with the target sample's
P0 true/false edges inverted. Production compiler/game code was not modified.
