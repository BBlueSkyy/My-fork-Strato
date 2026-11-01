# Readback critical-path diagnostic

This is an observation build on master + #409. It does not contain #411/#412's
product optimizations. It deliberately retains master's unconditional NCE write
routing. The #412 bounded ESR classifier is reused **only as an observer** in
`common/readback_fault.h`; its implementation is identical except for namespace.

Build with `-PreadbackDiagnostics=true` (ARM64 DevRelease). The CMake option
`STRATO_READBACK_DIAGNOSTICS` defaults OFF. On this dedicated diagnostic branch,
the PR workflow explicitly enables it. Existing Android flags and ABI are kept.
No new UI option, guest clock, fence, barrier, dirty decision or hack policy.

One reporter is owned by `OS::Execute`, not by GPU channels. It emits every five
seconds and is stopped/joined before logger finalization, also on exception
unwinding. Reporter output uses synchronous logging **off critical paths** so
no queued record retains a pointer to the exiting reporter's TLS. No logging,
explicit heap allocation, buffer hash or blocking diagnostic lock is added inside traps.

## Tests

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tests/readback_probe/run.py --deps app/libraries
ASAN_OPTIONS=detect_leaks=0:handle_segv=0 python3 tests/readback_probe/pipeline.py
ASAN_OPTIONS=detect_leaks=0:handle_segv=0 PROBE_ENABLED=0 python3 tests/readback_probe/pipeline.py
ASAN_OPTIONS=detect_leaks=0 SANITIZE=address,undefined CXXFLAGS=-DSTRATO_READBACK_DIAGNOSTICS=1 python3 tests/gpu_submission/run.py
ASAN_OPTIONS=detect_leaks=0 SANITIZE=address,undefined python3 tests/gpu_submission/run.py
```

ASan/UBSan are non-recovering. LeakSanitizer alone is disabled because the host
cannot inspect /proc under ptrace. `handle_segv=0` lets the real page-fault test
install its handler; ASan memory instrumentation remains enabled.

Optional native Vulkan (loader + headers + ICD + Khronos validation layer):

```sh
export VULKAN_HEADERS_INCLUDE_DIR=/path/to/Vulkan-Headers/include
export VULKAN_LOADER_LIBRARY=/path/to/libvulkan.so
# VK_DRIVER_FILES and VK_LAYER_PATH as needed.
ASAN_OPTIONS=detect_leaks=0:handle_segv=0 VULKAN=1 python3 tests/readback_probe/pipeline.py
ASAN_OPTIONS=detect_leaks=0:handle_segv=0 VULKAN=1 PROBE_ENABLED=0 python3 tests/readback_probe/pipeline.py
TEXMAN_TRAPS_ONLY=1 CXXFLAGS=-DSTRATO_READBACK_DIAGNOSTICS=1 python3 tests/gpu_backing_promotion/run.py
```

The native texture fixture needs the repository's Vulkan-Hpp/Boost include
paths and Clang (existing production designated-initializer order is rejected
by GCC). Existing suite runner accepts CXX/CXXFLAGS for those paths.

The buffer fixture selectively recovers #412's **test adapters**, then extracts
current production TrapManager/IntervalMap, both NCE observation and dispatch
expressions, staged Buffer callbacks/sync/invalidation. Shared guest/mirror
memfd mappings and CPU loads/stores cause real Linux x86 page faults. ARM64 ESR
records are synthetic translations of x86 fault metadata. This does not prove
ESR availability on Android. Vulkan buffers/fills/copies/fences are real; Buffer
allocator/FenceCycle interfaces remain host facades. Existing submission tests
separately compile production FenceCycle/SubmitInternal. Texture tests compile
production callbacks/conversion with real images and facade traps/allocator.

Both diagnostic ON/OFF preserve the original safe and unsafe-hack control
results. Mandatory waits and partial-store bytes must match. Unsafe stale reads
are expected **only in the negative hack control**, never accepted as a fix.
Core tests cover counters under concurrency, nested fence deduplication,
independent nested transfers, exceptions, bounded thread overflow, ESR eligibility,
slow-sample resource/cycle attribution, final-report lifetime, split-update
snapshot races and texture wait failure/retry. Reporter tests use actual fmt and
logger API signatures; an intentionally deferred TLS tag reproduces the review
UAF before switching the reporter to synchronous output.

## Reading `ReadbackProbe` logs

| Field | Meaning / limit |
|---|---|
| `window start/end/wall_ns` | CLOCK_MONOTONIC interval; comparable across threads in this process |
| `calls,bytes,ns` | Delta of cumulative counters; ns is completed **inclusive thread spans**, not frame/GPU time |
| `max_all` | Maximum since process start, not interval maximum |
| `h_us[...]` | Count histogram with upper bounds 1/10/100/1000/4000/16000/64000 us, then infinity |
| `kind_ns` | Span attribution: other / buffer / texture / presentation; nested categories overlap |
| `would412read,known_write,fallback_write_unknown` | #412's potential routing on real captured metadata; this APK still routes all accesses as writes |
| `no_esr,rejected_esr` | Missing/malformed ESR vs present ESR rejected by the oracle; diagnostic tags, not extra faults |
| `last_seen_esr,last_signal_code` | Independent last observations, not an atomic pair; the ESR may precede a missing-record signal |
| `trap_threads tid:completed_ns` | Top three threads by completed HandleTrap time; do not add thread times and call it frame time |
| `active_tid:age` | Outstanding trap span; can cross windows, not proof of GPU execution or deadlock |
| `slow_recent` | Top three durations among up to 128 recent spans >=1 ms; includes start time, kind, resource and cycle pointer |
| `slow_coverage` / `overflow` | Explicit retention losses / 64-thread attribution capacity; aggregate counters still work |
| `gpu_hz_point` | Read-only point sample from accessible Adreno sysfs; 0 means unavailable, not an idle GPU |

Reporter snapshots read independent atomics. A record can be split across
adjacent windows; **all changed fields are emitted**, even with zero new calls.
This avoids silent loss but does not provide transactional frame boundaries.
Same-thread nested FenceWait/SubmitWait/TrapDispatch are counted once per root.
Other metrics and different threads overlap. Never sum fence_root + mutex +
driver_wait + producer_wait + trap as if they were independent wall time.

`driver_wait` is CPU time inside vkWaitForFences, NOT GPU execution time.
`fence_mutex` can include a CPU blocked behind another thread already waiting
for the same GPU cycle. Compare slow samples' overlapping timestamps/cycle
pointers, not raw sums. Pointers are observation labels, not retained resources
or permanent IDs; do not infer identity across address reuse/nonoverlap.
`producer_wait` is separate from the additional texture `transfer_wait`.
`tex_readback` is an attempted operation duration (may fail), while
`tex_download_bytes` records successful surface payload after CopyToGuest.
`buf_readback`/`buf_upload` count exact mirror memcpy bytes. Staging surface
payload is not physical bus traffic. `tex_upload` is an upload preparation
attempt and may include wait/conversion; failed attempts are not GPU work.
`cpu_dirty` covers the buffer write-trap current-data path; `gpu_dirty` covers
staged buffer transitions. These are coarse observed spans, not precise dirty
interval ownership. `readback_request` counts eligible SynchronizeGuest calls;
actual copy counters distinguish no-op requests.

`present_request_dt` and `present_return_dt` use real-clock API boundaries.
They do NOT prove display scanout FPS. `guest_timestamp_dt` is the requested
presentation timestamp step, NOT simulation speed. No GPU timestamp queries,
full emulation CPU-frame timer or per-page dirty reconstruction are installed.
The five-second frequency sample cannot establish a governor ramp-up delay;
that needs an Android scheduling/frequency trace if these counters point there.

Signal/TLS boundary: Android uses the existing NDK r27 / minSdk29 native ELF TLS configuration. Probe TLS fields have constant initialization and no destructors; callback code makes no explicit allocator calls. First access can still involve Bionic TLS runtime initialization, so host tests do not prove zero Android TLS overhead. Existing guest thread startup calls AsyncLogger::UpdateTag before entering NCE (same native module TLS). Warm-up/device overhead control remains necessary.
