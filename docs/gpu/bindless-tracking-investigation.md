# Bindless CBUF handles spilled to local memory

The complete follow-up capture identifies a static CBUF handle spilled to shader
local memory. This branch resolves that proven case using the existing static
descriptor path. Android game execution and FPS comparison remain pending.

## Identified shader and correction

- Title: Jujutsu Kaisen: Cursed Clash, 010000B01A452000, v1.0.0.
- Compute shader hash: 01EBFC0F17AF7FA6; base offset 0x28AE1700.
- Local memory: 80 bytes; workgroup 64,1,1; shared memory 3328 bytes.
- Failing operation: BindlessImageWrite, Buffer, flags 0x00070000.
- Complete payload: 9720 Maxwell bytes and 335797 IR bytes, all chunks and End
  markers present, neither truncated.

In block 63, the exact value path is:

```text
%4229 = GetCbufU32 #2, #288
%4254 = Identity %4229
WriteLocal #13, %4254
... writes to words 4,5,0,1,2,3,14,15 ...
%4866 = LoadLocal #13
%4867 = Identity %4866
%4925 = Identity %4867
BindlessImageWrite %4925, %4924, %4930
```

Word 13 is byte offset 52, inside the declared 80 bytes. Both memory operations
are in the same basic block, and every intervening local store resolves to a
constant, different word. The handle is the unchanged CBUF2:288 value. There is
no evidence requiring GPU descriptor selection for this operation.

After the existing Track fails, TrackLocalCbufSpill checks this exact class of
value provenance: root scalar LoadLocal through identities, constant in-range
word, a reaching full-word WriteLocal in the same block, no possibly aliasing
intervening store, and a scalar GetCbufU32 with fixed bank/offset as its value.
It searches backward from the load, preserving SSA value timing even if another
store appears between the load and texture operation. Unsupported overwrites,
unknown aliases, absent definitions, dynamic offsets and inter-block memory flow
remain unsupported. The resolver never follows a loaded value's address to a CBUF.

The result uses ordinary static ImageBufferDescriptor creation and the existing
Vulkan binding. No runtime descriptors, GPU readback, waits, per-draw allocations,
new cache formats or cache invalidation are introduced. Previously successful
Track calls skip the new search completely.

The inspected Eden and fix/image-fetch-validation tracking paths do not model
this local-memory store/load edge. The latter's dynamic texture path also rejects
ImageWrite, so importing its broader Vulkan infrastructure would not resolve this
particular spill. No unrelated resources from that branch were imported.

## Focused verification of the functional change

A host fixture recreating the captured CBUF2:288 -> local word13 -> storage-image
write and all eight intervening stores reproduced the failure before the change.
After the change it produces one static image-buffer descriptor for CBUF2:288.
The CBUF read count matches a direct static-handle control: no extra reads are
performed by the resolver. Nine cases cover the captured shape, nearest matching
store, unknown aliases, unsupported overwrite, no definition, a memory-loaded
source whose address contains a CBUF, dynamic/out-of-range offsets, and a store
after the load that must not change the earlier loaded value.

An identical-harness comparison against master's pinned compiler dcb9388 ran 25
interleaved trials of 100000 successful static shaders: median 41.271 ms for
master and 42.286 ms with this change. Ranges were 36.887–51.875 ms and
37.099–52.052 ms. These overlapping, noisy host compilation timings do
not establish equal Android FPS. The static Track result remains immutable and
the resolver is isolated in the unlikely failure branch; no draw-time work or
IR/descriptor changes are introduced for already accepted handles. Device
performance comparison is still pending.

Only this demonstrated tracking failure has been addressed. The private Maxwell
capture is not committed. No claim of game boot, rendering, FPS, or complete GPU
bindless support is made without the next device test.

## Original evidence and reference comparison

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
None of that broader infrastructure was needed for the identified local CBUF spill.
Reference: https://github.com/BBlueSkyy/shader-compiler/tree/eb248b060d0aefa28a1d3028d461776fc92b9af4

## Failure-only capture

The compiler throws a dedicated BindlessTextureTrackingError with the failing
texture instruction address, handle instruction address/opcode, texture opcode,
type, depth bit, and raw instruction flags. TexturePass retains its IR in the exception while the
initial collection loop is still intact, before any texture opcode replacement.

Strato catches only that exception and adds stage, hash, base offset, texture CBUF
index, and graphics/compute environment parameters. It synchronously records shader identity before the same Maxwell
bytes that were passed to translation, including any configured replacement, and
then the IR retained in the exception. Only this fatal path uses synchronous
CPU file logging; it does not synchronize the GPU or alter the normal logger.
No guest memory or constant-buffer callback is read for diagnostics.

IR and Maxwell are hex-encoded in numbered, contiguous chunks in emulation.log.
Each payload has Begin/Chunk/End markers and explicit byte counts. Each is capped
at 2 MiB: **a capture is complete only when truncated=false and all offsets and
End markers are present**. Exceptions during capture preserve the original error.
A process kill or allocation failure can leave an incomplete capture.

Successful translation performs no logging or payload construction. The existing Track and TryGetConstBuffer logic, descriptor generation, GPU
bindings, synchronization, and all cache formats/keys remain unchanged; the new
spill resolver runs only when the original tracker returns no result. There is no game-ID check or cache reset.

## Verification performed

- Built production TexturePass and IR sources with GCC 13 on the host.
- A storage-loaded-handle reproducer failed against dcb9388 with only the generic
  error. With this diagnostic change it still rejects the same unsupported value,
  identifies the exact instruction, and exports reconstructable IR. This original diagnostic fixture
  is distinct from the local CBUF spill identified in the complete follow-up capture.
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
- Validated workflow YAML: one assembleDevRelease invocation, existing ARM64-only
  ABI filter, ccache/Gradle caches, one unconditional APK upload, no mandatory tests.

No Android SDK/NDK was available locally. The full APK is delegated to the PR
workflow; Actions is not monitored. No game execution or device performance
comparison was possible here. Keep both PRs Draft, with no merge.

## Next device validation

Run the new ARM64 DevRelease APK on the same Jujutsu scene and compare it with
master using the same driver/settings. Export emulation.log if another failure
appears. No cache deletion, game extraction or global tracing is required. Game
execution and device performance remain required before calling the task complete.

## Follow-up: incomplete first device capture

The attached emulation.log announced a 335797-byte IR snapshot but ended after
78336 bytes, before its End marker, original exception identity and Maxwell
payload. The initial asynchronous capture could lose its tail when execution
ended before the logger drained. At that checkpoint the actual failing instruction remained unknown;
the captured prefix alone was insufficient to choose a functional fix.

The compiler now attaches the IR to its dedicated exception instead of queuing
logs while unwinding. The host writes shader identity first, followed by Maxwell
and IR, through the existing synchronous file logger. Each chunk is flushed
before the error propagates. No global logging behavior changes. Missing snapshots
are explicitly marked IRUnavailable. A hard process kill during capture can still
interrupt it; validate the byte counts and End markers.

A host reproduction discarded the asynchronous queue after the producer returned:
the previous production payload helper lost its output; the revised helper
preserved all 8193 Maxwell bytes and a 335797-byte IR snapshot, reconstructed
byte-for-byte. The original compiler rejection and successful shader execution
semantics remain unchanged. ARM64 DevRelease-only CI is retained.
