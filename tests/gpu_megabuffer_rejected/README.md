# Oversized rejected views must not promote full-mirror copying

`python3 tests/gpu_megabuffer_rejected/run.py` extracts the actual Buffer validation,
reset, AdvanceSequence and TryMegaBufferView method bodies. Only Vulkan/engine
allocation interfaces are substituted; a CPU allocator performs actual byte copies.
It checks selected spans and immutable snapshot bytes, not GPU execution.

The regression assertion was first run against parent abf26faa and failed:
8 MiB rejected view followed by a 4-byte view copied **8,470,528 bytes**, expected
**4 bytes** for an offset-zero fixture. Removing only the rejected-size accumulation
makes it pass. A fixture with the observed physical size and real 13-bit table shift
also checks the observed small-view offsets 768/1024 (772 + 1028 copied bytes).

Other cases retain genuine full-copy promotion, large views after legitimate
promotion, reuse, cross-execution invalidation, same-execution AdvanceSequence
invalidation, eligibility for direct/staged/GPU-dirty resources, and the 192 KiB
inferred read size reported for Marvel. These are host mechanism tests, not device
FPS benchmarks or a Marvel graphics validation.

`SANITIZERS=address,undefined ASAN_OPTIONS=detect_leaks=0 python3 tests/gpu_megabuffer_rejected/run.py`
adds ASan. Leak checking is disabled because this host restricts task inspection.
