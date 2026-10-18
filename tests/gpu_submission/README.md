# Submission ordering host regression

Run `python3 tests/gpu_submission/run.py`. The default build enables UBSan.
`SANITIZE=thread` selects TSan; `ASAN_OPTIONS=detect_leaks=0 SANITIZE=address,undefined`
selects ASan and UBSan in containers where LeakSanitizer cannot inspect `/proc`.

The test compiles the production `FenceCycle` header, the entire production
`CommandExecutor::SubmitInternal`, and the entire production
`Texture::SynchronizeHostInline`, `Buffer::SynchronizeHost`, `Buffer::WaitOnFence`,
and `Buffer::UpdateCycle`. Vulkan calls and Android resource interfaces
use host facades. The synchronization algorithms, CPU-dirty state transitions,
staging cycle replacement, and dependency attachment execute production code.
Vulkan fence waits stop at an explicit gate; submission waiters must return
before that gate opens. A test-only callback in the list facade also gates the
pending-to-submitted race after its first predicate check and before its final
mutex acquisition. This rejects a submitted fast path that still shares the
GPU completion mutex. Promises establish thread entry, and bounded future waits
observe progress without sleeps. Cleanup always opens gates before join.

Checks cover submission waits and notifications while another thread is blocked
in `vkWaitForFences`, cancellation of both submission and pre-submission completion
waits, and ordering a staged texture upload before it overwrites its predecessor
cycle. Cancellation must leave the Vulkan fence unsignalled. The staged ordering
test requires only submission of prior work; its GPU fence remains unsignalled.

For a negative control, `SUBMISSION_BASELINE_REF=<git-ref>` compiles those same
production files from an earlier git ref against the identical assertions. The
unfixed PR #356 snapshot fails the eight affected assertions. To isolate the
buffer changes, `BUFFER_BASELINE_REF=<git-ref>` loads only `buffer.h` from that
ref; it reproduces the two missing clean/direct buffer submission waits.

Clean and directly imported buffers must retain predecessor submission ordering
when `SynchronizeHost` has no CPU upload to perform. Tests exercise the production
buffer methods through `SubmitInternal`, verify that the predecessor GPU fence
stays unsignalled, and check lifetime retention/release plus null/self-cycle
guards. CPU-dirty staged buffers already wait for completion in
`Buffer::SynchronizeHost` and retain that behavior.

These tests do not exercise a Vulkan driver, binary semaphore reuse, Android
command recording, or title performance.
