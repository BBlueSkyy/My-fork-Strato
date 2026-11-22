# Physical guest authority audit

Continuation of the existing Vulkan texture harness. No production changes.
The optional audit asserts the observed broken behavior and its positive
controls. Its success is **not a correctness pass for the emulator**.

```sh
# Configure the compiler/Boost/Vulkan paths as documented by run.py.
export CXXFLAGS="${CXXFLAGS:-} -fsanitize=address,undefined"
export ASAN_OPTIONS=detect_leaks=0
TEXMAN_PHYSICAL_AUTHORITY_ONLY=1 python3 tests/gpu_backing_promotion/run.py
TEXMAN_TEXTURE_REF=8453cc6f10500a7cf6a99bf14f85eaf66e50d489 TEXMAN_PHYSICAL_AUTHORITY_ONLY=1 python3 tests/gpu_backing_promotion/run.py
```

Base: master 8bcaee0be950b44234cdd2767a11578c74db2602. The second invocation
selects the texture methods from PR #480; the manager is identical on that ref.
This checks the *new overlap cases* despite the readback publication correction.
It does not rerun that PR's concurrent trap investigation.

Each run exercises five scenarios in both Free Guest modes and both image
format-list modes. Real RGBA8 images are rendered, read back and compared with
the exact expected native/block-linear bytes, including exclusive and shared
regions. Every producer fence is explicitly waited before the next consumer,
without encoding guest bytes. There are no pending executor nodes in this model.

Cases: cached A remains stale after B, a correct full-clear ordering control,
partial authority that whole-image serial sorting cannot represent, obsolete
guest overwrite before new C upload, and a contained non-mip producer omitted
from the fallback. The last has an explicit recovery/invalidation test control.
The 16384-byte fields delimit affected spans, not counts of unequal components.

The render helper now accepts an optional renderArea for a first-GOB-only clear;
existing calls retain their original full area. Sparse declarations in the
fixture match current production; unused sparse I/O fails closed. No existing
failure assertions were changed or skipped by the default runner.

Host boundaries: scheduler/allocator integration, traps and memory mirrors.
FreeMemory zeroes its mirror; it does not execute Linux MADV_REMOVE. There is no
Android, Turnip, game shader, full CommandExecutor or CPU trap concurrency claim.
Vulkan commands, render-pass clears, staging transfers, fences and native layout
encoders execute against the real software ICD and validation layers.

Full interpretation, limitations and the next bounded device experiment:
`docs/gpu/physical-texture-authority-20261122.md`.
