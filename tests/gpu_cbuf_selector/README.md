# Constant-buffer selector and MME regression tests

Run `CXX=/path/to/clang++ python3 tests/gpu_cbuf_selector/run.py`
(Clang C++20, ASan/UBSan; uses `clang++` by default). GCC is not supported
by the production macro interpreter's `always_inline` annotations.

This fixture compiles the actual selector/cache/bind and macro interpreter
bodies with host memory facades. It verifies clamped resident prefixes,
original bound bytes, P→Q reprepare, sparse/unmapped rejection, propagating
the original exception, and the captured castle macro's exact selector and
bind values. It does not enable or test temporary device diagnostics.

Historical captured macro controls can be replayed against either reference:

```sh
CBUF_CASTLE_REPLAY_REF=feb00c3d python3 tests/gpu_cbuf_selector/run.py
CBUF_CASTLE_REPLAY_REF=f3faa882 python3 tests/gpu_cbuf_selector/run.py
```

The original capture's D1D register value was not available; the fixture
uses 1 to choose the observed nonzero branch. See `gpu_gmmu_segment` and
`gpu_gmmu_identity` for the adjacent-unmap and mapping-generation regressions.
