# Storage-image descriptor preflight

Run `python3 tests/gpu_storage_preflight/run.py` (C++20, UBSan).

Compiles actual GetImageBinding and graphics full/quick/compute allocation/commit tails against host resource/allocator interfaces. Verifies that later descriptor retry and update-info allocation failure do not publish an image writer, while success commits it and read-only images remain read-only. The mechanism is selectively reused from #396; full draw/dispatch transactional preflight is outside this test and patch.
