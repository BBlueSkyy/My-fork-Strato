# Follow-up to BufferReadback v2

The new captures reproduce 6365 GPU-dirty preparation waits /48558734161 ns OFF
and1377 /14429061635 ns ON. Four reported IDs account for 43536381504 ns (89.7%)
OFF; IDs are local, never policy inputs. readback_wait=0 means preparation already
waited and cleared the cycle; it does not mean the GPU producer was free. released
0 OFF /2 ON shows the previous correction does not explain this workload.

Code path: writable storage/image binding, transform feedback, query or transfer
marks a buffer dirty. SubmitInternal attaches the entire execution's cycle through
Buffer::UpdateCycle, retaining predecessor submission ordering and lifetimes.
TrapManager retries a failed callback outside trapMutex. Buffer's preparation waits
that cycle; the following callback copies backing to mirror and preserves untouched
bytes before releasing guest permissions. Each fence wait can include predecessor
waits, submission, completion-mutex contention, driver completion and a semaphore
consumer cycle. The v2 logs do not distinguish these dependencies or writer origins.

Do not substitute last mark for last writer: cached writable descriptors/transform
feedback can keep writing without a new MarkGpuDirty call, and staged inline updates
can use a GPU copy without a new dirty transition. Coalescence also migrates dirty
resources. Dirty state plus a binding annotation cannot prove a newer cycle read-only.
Regional readback additionally needs the actual fault interval and authoritative GPU
write ranges; a descriptor's declared range is only a conservative bound. Anticipating
the memcpy alone cannot hide a producer whose bytes the faulting CPU instruction needs.

Chosen next step (authorized diagnostic fallback): retain v2 counters and add bounded,
per-buffer sampled decomposition of the *same* production fence waits, plus annotated
write sources/ranges. Do not change any wait, fence, trap, copy, generation or dirty-state
decision. Default builds stay probe OFF, existing PR ARM64 DevRelease enables it.

Plan:
- Add tests against the production FenceCycle for each blocking segment, cancellation,
  nested dependencies and concurrent waits; confirm missing diagnostic API first.
- Add an explicit optional per-call trace, no TLS/global caller state and no callback
  logging/allocation. Sample one of every16 GPU-dirty waits per admitted buffer. Root
  segments are disjoint within that sampled call; nested waits remain inside the root
  chain/tail segment. Keep global v2 totals unsampled.
- Validate ASan/UBSan, existing submission/buffer regressions and real Vulkan gated
  producer/readback. Review and commit/push this complete diagnostic step immediately.
- Annotate known GPU write sites with origin and conservative binding/transfer ranges;
  distinguish annotated marks from complete last-writer tracking. Validate/review and
  publish immediately in the same Draft. No speculative performance correction.

Risks/checks: root vs nested accounting; simultaneous waiter owns mutex; cancelled or
failed waits; trace pointer lifetime and no recursion through reporting; sampling/table
coverage; counters disabled in normal builds; aliases/recreation unmodified. Android,
Turnip behavior and title FPS require another actual device capture. Do not monitor CI.

Published phase step:034f1bc, same Draft#415. Production FenceCycle root phases,
per-call recursive trace and unchanged synchronization passed source review,
ASan/UBSan host submission/trace/reporter/buffer tests and native Vulkan writer,
copy/readback validation. Tests observe phase entry with a test-side clock wrapper,
using real monotonic timestamps; no production scheduling seam was added.

Writer step: annotations are diagnostic intent only, with conservative ranges and
unique cycle identities. Per-ID events start after table admission; last-annotation
snapshots captured under the buffer resource lock retain earlier one-shot bindings
without inflating event counts. Snapshot fields printed by the reporter are not
transactional. No writer generation, alias ownership, cycle attachment or dependency
ordering changes. Existing SSBO binding and query regression bodies compile and run
with probe ON/OFF; buffer annotation bodies preserve dirty state/cycle without waiting.

No performance fix for the dominant GPU-dirty path is established yet. Regional
tracking lacks actual fault/write intervals. Producer-only waits lack a complete
writer history (cached descriptors/transform feedback/inline updates/coalescence).
Earlier readback cannot consume bytes before the producing commands complete;
splitting a submission may help only if the new phase capture shows unrelated tail
work, and introduces measurable submission/ordering costs. The next v3 capture is
required before choosing any such structural optimization. No FPS improvement or
APK build success is claimed, and no Actions status is consulted.
