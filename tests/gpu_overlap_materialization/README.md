# Overlapping-texture materialization boundary

Run `python3 tests/gpu_overlap_materialization/run.py` (C++20, UBSan).

Compiles unchanged production source publication and retry-handler bodies against gated host resources. `OVERLAP_MATERIALIZATION_BASELINE_REF=9935f133` exercises the exact old fallback and fails because a new texture consumes the old producer image.

Covers pending-write receipts, submission before source readback, CPU lock contention, failure/retry and preserved eviction state. This is not a persistent authority ledger, full Texture/trap/fence implementation or device graphics test. The native Vulkan companion and still-failing #393 live-reuse oracle are documented in `docs/overlap-materialization-order.md`.
