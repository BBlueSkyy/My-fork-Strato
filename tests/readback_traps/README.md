# Precise NCE readback traps

`run.py` compiles **unmodified production** TrapManager + IntervalMap bodies,
both NCE TrapHandler call expressions, and (for `pipeline.cpp`) the staged Buffer
trap callbacks, SynchronizeGuest/Host, WaitOnFence/PollFence and invalidation.
Only foundation types, allocator/GPU interfaces and FenceCycle completion are
host facades. No copy of the algorithms is maintained in the fixtures.

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tests/readback_traps/run.py
ASAN_OPTIONS=detect_leaks=0:handle_segv=0 TEST_SOURCE=pipeline.cpp python3 tests/readback_traps/run.py
```

Default sanitizers: ASan + UBSan, non-recovering. LeakSanitizer is disabled in
this restricted host because its `/proc` inspection fails. `handle_segv=0`
lets the second fixture install its intentional page-fault handler; ASan memory
instrumentation remains active. That fixture is Linux x86-64 only. Actual mmap,
memfd shared guest/mirror aliases and CPU loads/stores exercise mprotect; x86
fault metadata is translated to **synthetic ARM64 ESR records**, not evidence
that a particular Android kernel supplied ESR. Both guest and host NCE call
expressions are checked separately. PROT_NONE translation syndromes and
read-only permission syndromes are tested; unknown contexts retain write routing.

Native Vulkan option (loader, headers, ICD and Khronos validation layer required):

```sh
export VULKAN_HEADERS_INCLUDE_DIR=/path/to/Vulkan-Headers/include
export VULKAN_LOADER_LIBRARY=/path/to/libvulkan.so
# Set VK_DRIVER_FILES / VK_LAYER_PATH when not installed system-wide.
ASAN_OPTIONS=detect_leaks=0:handle_segv=0 TEST_SOURCE=pipeline.cpp VULKAN=1 python3 tests/readback_traps/run.py
```

This enables synchronization validation and verifies real GPU fill -> fenced
readback -> partial CPU store -> GPU copy, comparing every byte. Facade
FenceCycle completion waits a real VkFence. It does not compile Android NCE or
Strato's full Vulkan scheduler; `tests/gpu_submission/run.py` separately covers
production FenceCycle/SubmitInternal dependency, cancellation and lifetime.

The existing native texture suite has a focused mode:

```sh
TEXMAN_TRAPS_ONLY=1 python3 tests/gpu_backing_promotion/run.py
```

It executes production texture callbacks/synchronization/layout conversion with
Vulkan images, aliases/contention, failure/recovery, registration after readback,
write gates, and dense Free Guest ON/OFF roundtrips. Texture trapping/allocation
are facades there; the new host suite independently exercises real TrapManager
page protections. The focused mode does not claim to pass unrelated historical
padded Free Guest fixtures in the full suite. Its clean no-op read-only fixture
explicitly cancels its unused command cycle (the prior investigation's known
fixture correction), without relaxing any production wait.

Negative controls, without modifying the working tree:

```sh
ASAN_OPTIONS=detect_leaks=0 TRAP_BASELINE_REF=fc8eb519 python3 tests/readback_traps/run.py protection
ASAN_OPTIONS=detect_leaks=0 NCE_BASELINE_REF=fc8eb519 python3 tests/readback_traps/run.py routing
```

The first must fail because the readback loses the next write trap; the second
must fail because NCE always selects the write callback. `pipeline.cpp` compares
A (legacy safe), B (legacy unsafe buffer hack, writes ON) and C (precise safe)
in one executable. The Clean-buffer wait case uses a second overlapping page
trap; standalone Clean buffers normally protect writes only. B is a negative control: a pending producer's old sentinel
must be observed, exposing its bypassed dependency. It is not a device A/B/C run.
