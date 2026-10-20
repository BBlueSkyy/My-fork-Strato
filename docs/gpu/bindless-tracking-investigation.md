# Unresolved bindless texture tracking: diagnostic checkpoint

This branch is a diagnostic checkpoint, not a functional fix for Jujutsu Kaisen:
Cursed Clash (010000B01A452000). The game must be run again to identify its actual
failing SSA expression. No new bindless execution path is enabled here.

## Evidence and reference comparison

The supplied emulation(20261020-112956).zip contains only emulation.log. It identifies
Jujutsu Kaisen v1.0.0 and ends at 71120239 with `Failed to track bindless texture
constant buffer`. There is no shader hash, stage, Maxwell payload, or IR in this
file. It also has earlier descriptor-pool errors; those do not identify the handle
expression. The other supplied ZIP (113657) is MY HERO ONE'S JUSTICE, not Jujutsu.

The compiler used by Strato master at 613d90dc is dcb938866f4234963fcbc199f2443438d8ef6f07.
In that compiler, MakeInst rejects when Track returns no ConstBufferAddr.
Track uses a breadth-first dependency search; TryGetConstBuffer models CBUF reads,
OR, AND, left shift, and a limited dynamic CBUF offset expressed as IAdd32.
The error text alone cannot distinguish a variable offset, memory-loaded handle,
immediate handle, or another SSA shape. A memory-load address must not be mistaken
for the loaded handle.

Eden source inspected at 989768aa46720e3fcab9e2111e3ddd70819c382b retains the same
MakeInst failure and breadth-first tracking, with separate cache/descriptor-stride
improvements. Its presence is not evidence that those changes fix this game.
Reference: https://github.com/eden-emulator/mirror/blob/989768aa46720e3fcab9e2111e3ddd70819c382b/src/shader_recompiler/ir_opt/texture_pass.cpp

The fix/image-fetch-validation compiler branch inspected at
eb248b060d0aefa28a1d3028d461776fc92b9af4 replaces arbitrary dependency traversal with
value provenance and preserves GPU-selected SSA handles in its dynamic path. It
also requires substantial matching Vulkan/runtime integration in Strato #365.
None of that broader infrastructure is imported without the actual Jujutsu shader.
Reference: https://github.com/BBlueSkyy/shader-compiler/tree/eb248b060d0aefa28a1d3028d461776fc92b9af4

## Failure-only capture

The compiler throws a dedicated BindlessTextureTrackingError with the failing
texture instruction address, handle instruction address/opcode, texture opcode,
type, depth bit, and raw instruction flags. TexturePass exports its IR while the
initial collection loop is still intact, before any texture opcode replacement.

Strato catches only that exception and adds stage, hash, base offset, texture CBUF
index, and graphics/compute environment parameters. It records the same Maxwell
bytes that were passed to translation, including any configured replacement.
No guest memory or constant-buffer callback is read for diagnostics.

IR and Maxwell are hex-encoded in numbered, contiguous chunks in emulation.log.
Each payload has Begin/Chunk/End markers and explicit byte counts. Each is capped
at 2 MiB: **a capture is complete only when truncated=false and all offsets and
End markers are present**. Exceptions during capture preserve the original error.
A process kill or allocation failure can leave an incomplete capture.

Successful translation performs no logging or payload construction. Track,
TryGetConstBuffer, descriptor generation, GPU bindings, synchronization, and all
cache formats/keys remain unchanged. There is no game-ID check or cache reset.

## Verification performed

- Built production TexturePass and IR sources with GCC 13 on the host.
- A storage-loaded-handle reproducer failed against dcb9388 with only the generic
  error. With this diagnostic change it still rejects the same unsupported value,
  identifies the exact instruction, and exports reconstructable IR. This fixture
  is not the unidentified Jujutsu shader.
- Reconstructed the complete fixture IR from log chunks; checked byte counts,
  failing instruction address, original opcodes, and zero diagnostic CBUF reads.
- Forced the log callback to throw: the original typed translation exception was
  preserved.
- Compiled the production Strato Maxwell payload helper with conversion errors
  enabled; reconstructed a non-aligned 8193-byte payload byte-for-byte and checked
  explicit truncation above the 2 MiB cap.
- Interleaved 15 runs of a 100,000-static-shader host probe against dcb9388 and this
  compiler: median 47.730 ms versus 46.851 ms, with ranges 39.031–61.753 ms and
  36.769–54.978 ms. No slowdown was observed in this noisy host probe; it does not
  establish Android FPS or game compatibility.
- Validated workflow YAML: one assembleDevReldebug invocation, existing ARM64-only
  ABI filter, ccache/Gradle caches, one unconditional APK upload, no mandatory tests.

No Android SDK/NDK was available locally. The full APK is delegated to the PR
workflow; Actions is not monitored. No game execution or device performance
comparison was possible here. Keep both PRs Draft, with no merge.

## Next required evidence

Run the ARM64 DevReldebug APK from this PR until the Jujutsu error and export its
normal emulation.log ZIP. No cache deletion, game-file extraction, or global shader
tracing is required. Use that capture to reproduce the actual SSA expression,
implement the smallest necessary static/dynamic support, and compare device
performance with master before calling the functional correction complete.
