# GraphicBufferProducer host regressions

Run from the repository root:

```sh
python3 tests/hosbinder/run.py
python3 tests/hosbinder/run.py --ref 3065a7e3
```

The runner compiles the complete, unchanged production implementations of
`QueueBuffer`, `CancelBuffer`, `DequeueBuffer`, `RequestBuffer`,
`SetPreallocatedBuffer`, `Disconnect`, and `GetPendingBufferCount`, together
with the production `BufferSlot` and `BufferState` declarations. Platform
resources, texture allocation, and the presentation sink have host interfaces;
the tested queue transitions and release callbacks execute production code.
The compiler enables undefined-behavior sanitization.

Tests hold presentation callbacks across preallocation and disconnect, then
reuse the same slot in dequeued and queued states. They also reset the texture
at the actual producer-mutex unlock boundary, exercise duplicate release of a
dequeued slot with the same frame number, and verify an ordinary frame's fence
still reaches presentation and the subsequent dequeue. The cancellation test
enrolls a real blocked dequeue thread, checks that the existing fence wait
still occurs before release, and requires cancellation to wake it. It uses a
bounded one-second failure timeout and a cleanup notification, without sleeps.

At master `3065a7e3`, seven regression cases fail and the ordinary fence case
passes. With the fix all eight cases pass.

## CPU synthetic benchmark

```sh
python3 tests/hosbinder/run.py --ref 3065a7e3 --benchmark
python3 tests/hosbinder/run.py --benchmark
```

Each command performs 10,000 warmup iterations followed by five samples of
1,000,000 iterations. An iteration calls the production `QueueBuffer` and
immediately executes its release callback. The fixture restores dequeued
ownership between iterations; texture allocation and asynchronous GPU work are
excluded. Both revisions use the same compiler flags and UBSan instrumentation.
Compare sample distributions and medians on an otherwise idle host.

This only measures CPU queue bookkeeping in the host harness. It does not
measure frame rate, GPU completion, device loss, or gameplay performance.
