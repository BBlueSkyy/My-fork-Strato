# Storage-image descriptor preflight

Run `python3 tests/gpu_storage_preflight/run.py` (C++20, UBSan).

Compiles actual GetImageBinding and graphics full/quick/compute allocation tails and operation commit method against host resource/allocator interfaces. Verifies that later descriptor retry and update-info allocation failure do not publish an image writer, while descriptor success still defers claims, operation success commits them, retained writes are repeated, and read-only images remain read-only. The mechanism is selectively reused from #396; full draw/dispatch transactional recovery is outside this test. The unpublished region continuation is blocked by the concurrency issue documented in `docs/region-authority-checkpoint.md`.
