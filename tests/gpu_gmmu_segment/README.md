# Real segment-table tail regression

`python3 tests/gpu_gmmu_segment/run.py` runs ASan/UBSan with the production
mmap-backed SegmentTable and GMMU map/unmap/lookup bodies. No new CI job.

Differential controls for the captured selector address:

```
GMMU_SEGMENT_REF=feb00c3d GMMU_SEGMENT_REPLAY_ONLY=1 python3 tests/gpu_gmmu_segment/run.py
GMMU_SEGMENT_REF=f3faa882 GMMU_SEGMENT_REPLAY_ONLY=1 python3 tests/gpu_gmmu_segment/run.py
```

The first passes; the second must fail at the expected null lookup. Default
current source additionally checks preserved dense/sparse tails before any
redundant remap, valid receipts, 512 raw-table mutation/byte-oracle steps and the
BufferManager pointer-table geometry.

`TEXMAN_SEGMENT_PROBE=1 python3 tests/gpu_backing_promotion/run.py` verifies real
Vulkan upload/write/readback and exclusive guest bytes through the same actual
table. It also runs inside the authority probe. The older broad mapping fixture
still uses a table facade; this targeted fixture specifically closes that gap.

The reduced owner uses a 36-bit address space, size_t and host mutex/span/error
adapters. The table's Set/lookup/allocation and map/translate bodies are unchanged.
This is a proven host regression with a matching device failure signature, not
proof that the device performed the exact test's preceding map/unmap sequence.
