# Guest/host page discard

Run `python3 tests/guest_discard_memory/run.py` on Linux. `CXX` can select Clang
or GCC. This compiles the complete production `FreeMemory`/`FreeHostMemory`
method bodies with a small host class/span/log adapter and UBSan. The existing
Android inline attribute warning is suppressed for host GCC.

A real shared memfd mapping verifies inward page boundaries and host pointers
with nonidentity guest translation. Private anonymous memory exercises failed
MADV_REMOVE; short spans exercise the no-op result. No APK, GPU or CI job is
required. This is not a test of texture overlap authority or allocation remaps.
