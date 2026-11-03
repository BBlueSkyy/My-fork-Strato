# Temporary constant-buffer selector failure capture

Run `CXX=/path/to/clang++ python3 tests/gpu_cbuf_selector/run.py` (Clang C++20,
ASan/UBSan; defaults to `clang++` when available). The production recursive macro
`always_inline` annotations require the Android/Clang compiler behavior; GCC is
not a supported compiler for this fixture.
No new CI job is required. See `docs/cbuf-selector-investigation.md` for the exact
production bodies, host facades, red controls and device reproduction procedure.

This verifies safe diagnostic capture and preserved rejection/data behavior. It
does not reproduce the reported Super Mario Odyssey crash or establish B1a as its
cause. The instrumentation never substitutes an unmapped constant buffer.
