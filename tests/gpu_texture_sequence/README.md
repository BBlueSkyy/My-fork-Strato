# Texture upload sequencing regression

Run `python3 tests/gpu_texture_sequence/run.py` (C++20, UBSan). Compiles unchanged production upload method bodies against host GPU I/O interfaces. Covers CPU authority, failure/retry, eviction ON/OFF and pre-submit lifetime retention; does not execute Vulkan or Android traps.

Negative control: `TEXTURE_SEQUENCE_BASELINE_REF=9935f133 python3 tests/gpu_texture_sequence/run.py` fails because the failed upload loses CPU authority.
