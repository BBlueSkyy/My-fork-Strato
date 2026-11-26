# Occlusion-query host regression checks

Run from the repository root:

```sh
python3 tests/gpu_queries/run.py
```

Requires Python 3 and g++ with C++20, AddressSanitizer and UBSan support. The
runner extracts the production `Queries` declaration and implementation unchanged,
plus the actual helper shader's `Resolve`/`PrepareForTransfer` recording methods.
It also compiles the unchanged GLSL `add64` and `main` function bodies with host
scalar/vector and storage declarations, so carry checks execute production
arithmetic rather than a separately written summation algorithm.

Host interfaces replace Vulkan handles, allocation, descriptor construction and
command execution. The deferred executor models pre-execution resets, render-pass
commands, pre-exit hooks and post-pass work. Its recording interface rejects
queries begun/ended in invalid scopes, copies before query end, destroyed buffer
handles, and selected buffer access hazards lacking matching stage/access and buffer-range barriers. Fake Vulkan
handles retain no resource ownership; cycle dependencies and recorded callbacks
must retain the real production resources. The hazard checker covers the tested
buffer dependencies; it is not a Vulkan synchronization validator.

Cases cover:

- Enabled/disabled/enabled draws, query begin/end placement and render-pass changes.
- Reset/report transitions, repeated reports, and reports between draws.
- A later command consuming a report before the next draw or render-pass exit.
- 32-bit truncation, untouched following bytes, 64-bit carry and timestamps.
- More than 1024 segments, unsignalled-bank isolation, signalled-bank reuse and
  persistent accumulation across executions.
- Empty resets following transfer reads or compute writes, repeated writes to
  a report destination, and report writes between arbitrary earlier/later
  consumers. Report buffer barriers are checked with a nonzero destination offset
  and the full result-plus-timestamp range.
- Destruction of `Queries` before deferred command recording, including an empty
  report with no query bank, and accumulator retention until cycle completion.

The tests initially reproduced missing reset/report barriers, premature
accumulator destruction, and a report queued behind its later consumer. The
runner enables ASan and UBSan; leak scanning is disabled because the host runner's
ptrace environment prevents LeakSanitizer from enumerating threads. These checks
do not establish Android build success, SPIR-V compilation, Vulkan validation or
rendered pixel correctness. They need no game files or GPU device.

Optional checks use actual Vulkan interfaces and a headless device:

```sh
python3 tests/gpu_queries/compile_vulkan.py --required
python3 tests/gpu_queries/run_vulkan.py --required
```

The compile check needs the pinned Vulkan-Hpp and nested Vulkan-Headers
submodules. It compiles the query implementation, helper pipelines and Maxwell
conditional methods against real Vulkan types, substituting engine interfaces.
The integration check needs Vulkan headers/loader, glslangValidator, spirv-val,
an ICD (Mesa lavapipe is sufficient) and VK_LAYER_KHRONOS_validation. It executes
the production shaders and barrier masks, query-pool reuse and triangle predicates
with the conditional extension enabled and disabled. Omitting `--required` permits
these optional runners to skip when their prerequisites are missing.
