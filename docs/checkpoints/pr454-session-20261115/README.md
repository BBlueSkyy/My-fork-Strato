# Recovered Work checkpoint — 2026-11-15

All prior local changes and fixtures were recovered before further implementation.
PR454 remains at d951ca35 and diagnostic PR455 at 3087d5a5; neither is overwritten.

The archive preserves prior test logs and replay proof/summary metadata. Original
user PCHE/Maxwell inputs and generated SPIR-V binaries remain in the private
workspace; no game binaries are published in this repository.

Last results: original four cache failures reproduced; later snapshot/cache tests
and 1,117 TIC checks passed. Real Vulkan retained ten pipeline variants across an
old pending fence with zero validation errors. The next equivalent-handle test
correctly reports TWO failures: the naive scalar snapshot recompiles when only a
texture handle changes to another texture with equivalent queried metadata.

The compiler query-origin metadata interface was started but renderer integration
is unfinished in this checkpoint. Its checkpoint commit preserves that work; it
is not a claim of a compiler code-generation defect or a passing release build.
No new instrumentation was introduced. The full original Kirby replay remains
verified and its SPIR-V hashes are recorded in docs/pr454-shader-replay.md.

Published compiler checkpoint: cb0ea66d573b3b58415b51f8dac0f18d82557b01. Its tree matches local 32ca7d7 exactly; local checkpoint refs are retained.
