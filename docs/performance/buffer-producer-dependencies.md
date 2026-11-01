# Producer-scoped staged buffer readback

Final captures (SHA256 OFF e30e3c38c66339559950e5393245e0b2c098a18c2becf8077a2a613dd4ae78e4,
ON ff887d5a1b974d1be4822287d9afb087a239cc1e6db507e4a2d74b063c07a29d):
OFF175.060s,8838 dirty_prepare/69.585s cumulative. ID92986,016B contributes
2336 waits/40.018s;2269 downloads/195170304B cost0.272s CPU memcpy. In its146
samples,145 cycles differ from last annotated compute intent; chain0.334s,
mutex1.168s,driver0.414s,submit0.030s,total1.953s. These are sampled inclusive
thread wall times, not frame time or proof that a mutex wait is avoidable.
ON115.034s is a different duration; shortcuts copy potentially stale bytes.

Hypothesis supported by the code: Buffer::cycle is the latest *use* cycle,
including read-only users after a GPU writer. Staged readback only reads the
backing into a separate mirror, so simultaneous later GPU reads do not conflict.
A later CPU-to-backing upload still conflicts with all those readers.

Design (no device instrumentation): retain the existing cycle for submission
ordering, full backing-write waits and lifetime. Add a producer cycle for guest
readback only. Capture it from a pending writer flag at UpdateCycle, defaulting
to the latest cycle when producer provenance is unknown. Unlike transient barrier
state, the pending writer flag survives recursive resource unlocks. GPU-dirty inline copies
must set the writer flag even without a new dirty transition. Preparation retries
must revalidate the selected producer after dropping the resource lock to wait;
a new writer must force another wait. Never clear an unrelated latest-use cycle.
SynchronizeGuest still copies every byte before cleaning. SynchronizeHost, direct
imports, barriers, aliases and coalescence retain full-use synchronization.

Writer coverage to verify: compute descriptors every dispatch; graphics/transform
feedback re-flushed after each executor submission (including cached bindings);
queries; DMA and inline GPU copies; repeated GPU-dirty writes; dirty coalescence.
Do not use diagnostic annotations or per-game IDs as authority.

Implementation plan:
1. Add a production-body writer->pending reader->CPU partial-store regression;
   run against current code and observe redundant blocking. Add native Vulkan
   counterpart with gated reader, byte comparison and later upload wait.
2. Implement producer capture, readback wait selection and retry validation;
   include GPU-dirty inline writes. Retain full waits on backing mutation.
3. Validate a newer producer, concurrent replacement during wait, failure/retry,
   unknown producer fallback, aliases/shared mappings and backing invalidation.
   Check cache-flush writer coverage and production submission ordering.
4. Run ASan/UBSan, native Vulkan synchronization validation, independent source
   review; commit/push immediately in PR415 after essential checks pass.
5. Existing ARM64 DevRelease workflow builds functional probe-OFF APK. Never
   consult Actions. Moto G100 FPS/graphics/simulation validation remains external.

Implemented invariants:
- gpuWriteCycle is distinct from the unchanged latest-use cycle. A dedicated
  pendingGpuWrite flag is set by every MarkGpuDirtyImpl and by actual GPU-dirty
  inline/CBUF callbacks, and consumed only at UpdateCycle. Nested unlocks cannot
  discard it; pending/unsubmitted writers retain conservative full-use selection.
- UpdateCycle still waits for predecessor submission and chains all predecessor
  lifetimes. Direct imports keep existing behavior. Graphics flush invalidates
  active descriptors, all state (including transform feedback) and quick bind on
  every Submit; compute synchronizes descriptors on every dispatch. Writable
  SSBO and image/texel buffer bindings, queries and DMA still call MarkGpuDirty.
- The prepare callback rechecks the selected producer under resource/state locks
  after waiting outside them. SynchronizeGuest waits that producer and copies the
  entire backing before cleaning. It leaves later reader cycles intact.
- SynchronizeHost, WaitOnFence/PollFence, coalescence and backing destruction keep
  full-use dependencies; successful completion clears producer state, failed waits
  retain it. No fences, barriers, commands or transfer bytes are removed.

Verification: old44b1b05 production bodies fail both host and native Vulkan tests
because readback waits for an event-gated later reader. Candidate completes that
readback after the real producer, with the reader still pending. Its CPU store
changes mirror only; upload still blocks on the reader. Every byte is compared
for the reader's old version and a subsequent GPU consumer's updated version.
Native synchronization validation is clean on llvmpipe. The gates deliberately
create a dependency delay; they do not measure Zelda frame time or GPU speed.

ASan/UBSan tests cover producer/read-only reader, partial CPU writes, a newer writer
attached during the outside-lock wait, tracked and unknown-producer failure/retry,
AllWrites/nonblocking fallback, full poll cleanup then a new writer, GPU-dirty
inline/CBUF replay and recursive unlock. Existing production submission-order,
conditional writer/generation and optional reporter tests remain applicable.
The native harness executes production Buffer methods around real VkFence waits;
resource-allocation/fence interfaces and NCE contexts use documented host adapters.
A separate native test exercises production FenceCycle itself. No claim of full
Android or arbitrary TextureManager alias validation is made.

Independent review found no blocking issue. Remaining limit: this separates
later read-only executions, not commands within the producer submission or
necessary predecessor/semaphore dependencies. Mutex time can still be GPU wait
performed by another waiter. Device40.018s is not a promised saving; no FPS gain
is claimed until Moto G100 testing. No additional device diagnostics were added.
The existing ARM64 DevRelease build now explicitly disables the optional probe.
Existing diagnostic source/files are preserved; Actions is not consulted.
