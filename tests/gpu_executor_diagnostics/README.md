# Executor diagnostic host regression

Run `python3 tests/gpu_executor_diagnostics/run.py` from the repository root.
Requires Python 3 and a C++20 compiler with UBSan. `CXX` selects the compiler.
`STRATO_TEST_REF=HEAD` selects an earlier production `command_executor.cpp` from
git while retaining the same fixture and assertions.

The runner extracts the complete, unchanged production
`CommandExecutor::AddSubpass()` and `AddFlushCallback()` and includes the real
`pacman_triangle_capture.h`. The tests exercise diagnostic mode selection,
post-candidate counting, draw publication before callbacks, preservation of
callback-appended nodes/resources, natural threshold submissions, the
next-subpass and callback-reentry guards, modes0 through15, and mode16 persistence
after 60 presents.
Threshold cases cover both callback-appended query nodes and nested draws:
submission must wait until the callback bundle exits, then include its new work.
Mode17 cases register a graphics callback at a shuffled index and verify only
that callback runs, general callbacks cannot publish query nodes, unavailable
graphics callbacks do not count as interventions, and mode16 still runs all
four callbacks. Both modes retain their selection after the capture closes.

The renderpass builder is a facade with an explicit next-subpass result.
Vulkan objects are metadata-only facades; formatting is a no-op. `Submit()` is
an observable host boundary, not the production submission implementation. It
records calls and simulates rotation, tag advancement and resource reset so an
accidental forced submission is detected. These tests do not validate Vulkan
synchronization, framebuffer contents, real flush callback implementation,
GPU resource lifetime or PAC-MAN visual correctness.
