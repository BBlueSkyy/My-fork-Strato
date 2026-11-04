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
