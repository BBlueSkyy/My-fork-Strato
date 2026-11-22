# Guest readback publication

Selective continuation of the test harness from PR #392/#395, using the current
production SynchronizeGuest, read/write traps and blocking lock callback. GPU I/O
is a gated host facade; these tests do not execute the game or certify its menu.

```sh
SANITIZE=address,undefined python3 tests/gpu_guest_readback/run.py
TEXTURE_READBACK_REF=8bcaee0be950b44234cdd2767a11578c74db2602 READBACK_TRAPS_ONLY=1 SANITIZE=address,undefined python3 tests/gpu_guest_readback/run.py
READBACK_TRAPS_ONLY=1 SANITIZE=address,undefined python3 tests/gpu_guest_readback/run.py
```

The baseline command is an intentional negative control. All eight combinations
of read/write callback, freed/resident bytes and readback hack ON/OFF grant access
while the copy is gated and the guest value is still 7 instead of 42. The corrected
command rejects all eight callbacks while gated, then retries successfully after
copy completion. No timing-dependent sleep is needed: promises gate the copy.

The full suite also covers retained guest validity through six injected I/O
failure stages with and without aliases, success/retry, unsupported conversion,
undefined images, preserved converted sampled-only guest data, read-only/stale
aliases, generations and the mapped-image path. The mapped-image test uses a
facade CopyToGuest; it is not proof that every real encoder layout is correct.

Additional real-Vulkan audit, using the existing backing-promotion harness:

```sh
TEXMAN_AUTHORITY_ONLY=1 python3 tests/gpu_backing_promotion/run.py
TEXMAN_FREED_ONLY=1 python3 tests/gpu_backing_promotion/run.py
TEXMAN_SKIP_KNOWN_FREED=1 python3 tests/gpu_backing_promotion/run.py
```

`TEXMAN_AUTHORITY_ONLY` runs 14 scenarios in each format-list mode: recoverable
writer before/after readback, unsupported writer, no usable writer, stale writer
and divergent/published generations, and a padded layout. It records the current
implicit-layout rejection and explicit-layout behavior; it does not claim to fix
those paths. Native Vulkan render/copy/encoding is real, but the fixture's
FreeMemory facade zeroes the entire mirror rather than reproducing Linux page
rounding. Its padded case loses 24576 sentinel bytes under that model. The older
PR #395 separately proved padding loss with real memfd/MADV_REMOVE.

`TEXMAN_FREED_ONLY` intentionally retains the preexisting failing
CheckFreedWriterGuestBridge test. The correction does not remove its implicit
memoryFreed rejection. The last command skips only that known failing case,
explicitly; do not report it as an unqualified pass of the complete suite.

The fixture was brought up to the current sparse interface (unused sparse I/O
fails closed), and Clean-only probes now cancel the command buffers they never
submit. Two old assertions expecting Clean after an unsupported writer readback
now require GpuDirty; the original rejection and byte-preservation checks remain.

Dependencies: C++20 compiler, Boost, Vulkan headers/Hpp, loader, software ICD and
Khronos validation layers for backing tests. CXX/CXXFLAGS and the backing runner's
VULKAN_* environment overrides support nonstandard installations. Disable LSan
only if the host cannot support it; ASan/UBSan still run.
