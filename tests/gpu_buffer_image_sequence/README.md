# Raw buffer/image transfer substrate

`python3 tests/gpu_buffer_image_sequence/run.py` uses the existing real Vulkan
fixture and `texture/buffer_image_copy.h`. Native RGBA8 sectors in a block-linear
GOB become bounded 4×1 `VkBufferImageCopy` rectangles; the same plan works in both
directions. No compute shader, CPU detiling or padding interpretation is needed
for this transport.

The oracle calculates byte addresses independently. It checks a GPU-written nonuniform raw
buffer → image upload, a second overlapping one-GOB producer, a separate image
write, reverse image → buffer transfer, untouched buffer guards, actual Free
Guest OFF/ON eviction followed by production guest readback/publication, and
format-list OFF/ON. Invalid bounds, alignment, overlaps and the 128-GOB/4096-copy
ceiling are rejected before recording. `BUFFER_IMAGE_LINEAR_NEGATIVE=1` substitutes
a plain linear upload and must fail the real-byte comparison.

This is **not** an automatic alias-coherence test: it does not register a guest
Buffer as a regional authority participant. The Vulkan buffers are transport
resources in the host fixture. The existing image authority A→B→A test remains
the sequencing/eviction regression gate. Shared buffer/image authority cannot be
enabled until selective buffer publication, producer commit, cached binding
preflight and coalescing migration are integrated together.

The host driver is lavapipe with Khronos synchronization validation. `SANITIZE`
selects the fixture sanitizer (default `undefined`; `address,undefined` is also
supported). Tests remain opt-in and no APK workflow/CI job is added.

Automatic mixed authority is a separate mode:

```sh
SANITIZE=address,undefined ASAN_OPTIONS=detect_leaks=0 LP_NUM_THREADS=2 \
  python3 tests/gpu_buffer_image_sequence/run.py --automatic
```

It extracts production Buffer, managers, typed region reconciliation, executor
preparation/publication/retry, and TrapManager dispatch bodies. The production
activation constexpr is enabled only in a temporary header copy. Cases include
partial overlaps and exclusive bytes, partial-GOB buffer writes, multiple image
producers, queued retained consumers, CPU read/span/write/copy APIs, all-entry
CPU trap dispatch, failures/retry, writer conflicts, mapping lease rejection,
coalescing and independent-group premerge. The reviewed attached-source lifetime
and already-published foreign-image guest-byte coalesce cases are mandatory.

`TEXMAN_MIXED_GATE_OFF=1` deliberately disables registration in that copied
header; the automatic suite must fail its tracked-resource assertion. The
`TEXMAN_MIXED_COALESCE_REVIEW_C1=1` and `..._C2=1` environment controls isolate the
attached-source and published-foreign-cell coalesce cases for RED→GREEN work.
DMI registration rejection is tested using a staged allocation with an admission
flag; actual Android DMI transport is not supported by this host fixture.
