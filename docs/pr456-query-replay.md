# Recovered texture-query interface: shader byte identity

The recovered compiler checkpoint
`cb0ea66d573b3b58415b51f8dac0f18d82557b01` produces **byte-identical complete
SPIR-V modules** for both captured Kirby fragments compared with the preserved
`e972cae1d8de3c437769bb2b34fd23af7e030c40` replay. The source-tracking interface
does not change their emitted shader code under this replay's declared profile.

This verification continues the preserved checkpoint. It does not replace the
original replay or change any compiler/product source. The baseline inputs,
offline profile, structural proof, and device-execution limits are documented
in [the original replay report](pr454-shader-replay.md).

## Verified result

| Fragment hash | SPIR-V bytes | Unchanged SPIR-V SHA-256 |
|---|---:|---|
| `E7582A61F9097487` | 4216 | `6d8b9888ccc0694bf3ebacae097c32cbd495699c167b7ff56b0c6104aec12dda` |
| `BC3F7489079E8ABE` | 4076 | `5279dfabd3da6d9fb30a598f260ec303585681ee7d32ca6eb16ea631c324311d` |

Both freshly emitted modules pass `spirv-val --target-env vulkan1.1spv1.4
--uniform-buffer-standard-layout`. The full SPIR-V bytes, raw-ID disassembly,
descriptor list, extracted Maxwell binary, environment input, compile log,
validator/disassembler logs, and replay summary match the preserved baseline.
The complete IR text matches after removing only the leading host-pointer
annotations printed by `frontend/ir/basic_block.cpp`; those process addresses
are the sole IR-text difference.

`prove.py` passes both through `run.py` and its standalone CLI. `CBUF3[4]`
remains a live Uniform load used by the unordered comparison. It is absent
from compile-time CBUF callback logs. All six samples remain: the sample
associated with `CBUF2[0x30]` is reachable only on P0's true edge; +0 and -0
skip it, while +1, -1 and NaN can reach it. The complete generated modules,
rather than just this predicate, are byte-identical.

## Callback contract and build provenance

The preserved `replay/test_query_callbacks.py` compiles and runs against the
actual compiler `Environment` header with assertions and UBSan enabled. It
passes all four query kinds: type, pixel format, integer format and comparison.
The default callback retains two CBUF reads in the original order, including
duplicate reads when sources coincide, and preserves shifts and bitwise OR.
The tracking override records both source locations and query kind, returns
the same metadata, and invokes no scalar CBUF callback.

This callback test uses a focused test override. The full fragment replay uses
the actual default `Environment::QueryTexture`; it does not instantiate the
Android renderer's new override. Renderer invalidation behavior needs its
separate integration checks. Neither test claims device execution or proves
that an invalid TIC is safe to sample.

The compiler worktree was clean at `cb0ea66`. A new build directory compiled
all 159 translation units with two workers; no baseline objects were reused.
The manifests confirm identical host compiler and matching dependency
commits/status. Of the translation-unit source hashes, only
`ir_opt/texture_pass.cpp` differs from the original build; compiler headers
are also hashed and `environment.h` contains the new interface. Both runs use
the same PCHE v5 input with SHA-256
`11868f75aea624a8294907d64d0566f19b6ad85422e32922bf28ed66f856fb5b`,
and all 1270 bundle checksums pass.

## Reproduce without overwriting the baseline

Use the dependency, compiler and input variables from the original replay
report, with these separate output paths:

```bash
REPLAY_BUILD_QUERY=/workspace/scratch/33440eae037b/kirby-replay-build-query
REPLAY_OUT_QUERY=/workspace/scratch/33440eae037b/kirby-replay-results-query
python3 tests/gpu_tic_validation/replay/test_query_callbacks.py \
  --deps "$HOST_LIBRARY_DEPS" --boost "$BOOST_INCLUDE"
python3 tests/gpu_tic_validation/replay/build.py \
  --deps "$HOST_LIBRARY_DEPS" --boost "$BOOST_INCLUDE" \
  --build "$REPLAY_BUILD_QUERY" --jobs 2
python3 tests/gpu_tic_validation/replay/run.py \
  --cache "$USER_STAGING_CACHE" --binary "$REPLAY_BUILD_QUERY/replay" \
  --out "$REPLAY_OUT_QUERY" --spirv-tools "$SPIRV_TOOLS_BIN"
cmp "$REPLAY_OUT/E7582A61F9097487.spv" \
  "$REPLAY_OUT_QUERY/E7582A61F9097487.spv"
cmp "$REPLAY_OUT/BC3F7489079E8ABE.spv" \
  "$REPLAY_OUT_QUERY/BC3F7489079E8ABE.spv"
python3 tests/gpu_tic_validation/replay/prove.py \
  "$REPLAY_OUT_QUERY/E7582A61F9097487.spvasm" \
  "$REPLAY_OUT_QUERY/BC3F7489079E8ABE.spvasm" \
  --out "$REPLAY_OUT_QUERY/predication-proof.json"
```

The isolated build contains `build-manifest.json`; its output directory
contains `replay-summary.json`, `byte-identity-report.json`,
`predication-proof.json`, `proof-check.log`, `callback-contract.log` and all
module artifacts. The original `kirby-replay-build` and
`kirby-replay-results` were preserved.
