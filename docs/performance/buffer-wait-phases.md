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
