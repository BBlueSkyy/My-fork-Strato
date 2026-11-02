# Host draw and conditional-rendering regressions

Run from the repository root:

```sh
python3 tests/gpu_conditional/run.py
```

The runner extracts and compiles the production `Maxwell3D::Draw` body,
conditional-rendering methods, synthetic-index trace helper,
`ActiveState::InvalidateIndexBuffer`, and `dirty::ManualDirtyState` wrapper.
The shader comparison expression and buffer-write generation methods also
come from production. AddressSanitizer and UndefinedBehaviorSanitizer are enabled.

The synthetic-index regression starts with capture unarmed and experiment mode
zero, warms a clean guest index cache, then calls the complete production
`Draw` body with an originally nonindexed four-vertex quad. It observes the
synthetic index binding, then verifies a following indexed draw restores the
retained guest binding without purging its view or forcing submission. Separate
controls verify ordinary draws and already-indexed draws (including quad
conversion) preserve the clean index cache.

`PrepareDraw`, the cached guest index view, quad-buffer allocation/conversion,
state-command recording, and Vulkan execution are host facades. The cache
sentinel emits its retained binding only when the actual dirty wrapper calls
`Flush`; this checks the production draw-to-invalidation connection rather
than a call count. Actual `IndexBufferState` preparation and mapped-view behavior
are covered separately by `tests/gpu_index_binding/run.py`. This suite does not
execute `DrawWithInlineIndex`, a Vulkan driver, or a GPU, and does not
establish rendered-image correctness.
