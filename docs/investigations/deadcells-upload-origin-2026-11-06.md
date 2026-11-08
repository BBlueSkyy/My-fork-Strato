# Dead Cells — display upload provenance probe (diagnostic only)

The RenderDoc frame shows a corrupted RGBA8 1280×720 image before presentation.
Its source Vulkan upload buffer and destination image match byte-for-byte.
That does **not** identify the producer of the buffer; this temporary branch
investigates the upstream CPU-to-Vulkan texture preparation without changing
presentation, synchronization, resource ownership, or shader compilation.

## Capture scope

- **Build:** based on the unchanged master `e6da30c3799fd12549143a982b04ab14d1c2998f`, not on Draft PR #400.
- **Candidate:** 1280×720, single mip/layer, 3,686,400-byte RGBA8/BGRA8 host texture.
- **Provenance:** `Texture::SynchronizeHostImpl()` records the raw guest mirror bytes and the final prepared upload bytes, with guest tile mode, pitch, strides, formats, per-process texture pointer, aliases, and two FNV64 checksums.
- **Selection:** inspect up to 120 candidate uploads; save up to four uploads where fewer than 10% of output alpha bytes are 255, plus two reference uploads. At most 12 binary files, each at most 8 MiB.
- **Presentation:** record the per-process texture identity selected in `PresentationEngine::PresentFrame()` for up to 24 present calls.
- **Destination:** `getPublicFilesDir()/logs/deadcells-probe/` (JSONL and binary files). The data is reset on a new game launch. This diagnostic branch writes automatically; do not merge it as production code.
- **No inferred IOVA:** `GuestTexture::mappings` are spans into CPU mappings, not guest IOVA. Correlate to IOVA/TIC only with a supported Strato sidecar/debug name.

## Moto G100 test without ADB

1. Install the ARM64 DevRelease APK from this Draft PR.
2. Start Dead Cells **once**; reproduce the colored noise before its title menu.
3. Exit the game and return to the Strato game list (do not restart the game before export).
4. **Long-press the existing logs icon** in the search bar to share `deadcells-probe.zip`. A normal tap still shares `emulation.log`.
5. Send the probe ZIP with the captured RenderDoc RDC/AI ZIP already provided. The probe ZIP also includes `emulation.log` if it is within the size budget.

## Interpretation and limits

- A prepared staging sample matching the RenderDoc corrupted buffer identifies a path in Strato for upstream tracing.
- Raw guest and prepared staging bytes are intentionally **not** expected to match when swizzle/pitch or host format conversion is used.
- A raw guest layout that becomes invalid only after preparation suggests layout conversion or stale guest mapping; it still requires a controlled A/B or producer trace before a code change.
- FNV64 is a cheap diagnostic correlation hash, not cryptographic proof. Compare the exported binaries directly.
- An unexpected texture, a producer GPU write, or a different/earlier submit can remain outside this narrow probe.
- Logging/dumping can add startup latency. No CPU/GPU waits or barriers have been added. Do not infer root cause from the alpha percentage alone.

## First Moto G100 evidence — probe ZIP (2026-11-06)

- Driver: Turnip 24.0.99. The ZIP contains 10 logged uploads and 6 guest/staging binary pairs; no archive corruption.
- Every recorded guest texture is 1280×720, RGBA8 (Vulkan format 37), block-linear (tile mode 2), with guest allocation 3,932,160 bytes (padded) and linear staging 3,686,400 bytes. No sample reports an alias or freed guest memory.
- Independent Tegra GOB sector-order reconstruction (block height 16 GOBs, GOB 64×8 bytes/lines) reproduced **all 22,118,400 staging bytes exactly** from the six captured guest binaries. This rules out a mismatch in the observed block-linear deswizzle output; it does not prove the source guest contents are valid.
- Samples 1, 2 and 5 were blank in both guest and staging; using three precious capture slots for zero buffers blocked interesting later samples. Samples 3, 4 and 6 produced visible noise/tile-like patterns.
- A particularly valuable unsaved upload is **#7**, reporting 8,447 alpha=255 pixels of 921,600. The earlier independently captured RenderDoc event 1380 buffer had **exactly 8,447** such pixels, though the respective FNV64 hashes differ. This is a fingerprint candidate, not proof of resource identity across boots.
- Latest diagnostic probe now reserves the capture slots for **nonblank** uploads and keeps the first 600 present-texture identities instead of only 24.
- Errors in the same game run: **142** unsupported layer-create warnings, **5** `QueueBuffer` state warnings (`Free` instead of `Dequeued`), **5** matching `CancelBuffer` warnings, and a Turnip `VK_ERROR_OUT_OF_POOL_MEMORY`. None is yet causally linked to the pixel corruption.
- The original RenderDoc buffer does not match any of the six saved staging binaries byte-for-byte (different launch). Reproduce the same glitch with this revision and compare the new, nonblank #7-like guest/staging dumps.

The next trace should identify the producer/write site of the selected guest memory and correlate buffer-queue presentation identities; do not modify the render graph or resource sequencing before that evidence.


## Second Moto G100 evidence — near-identical pixels across boots

The updated ZIP includes samples 3, 4, 6, 7, 8 and 10 and 281 display-presentation records. The critical link is **sample #7**:

- `sample_7_staging.bin` vs earlier RenderDoc `upload_1380_buffer.bin`: **3,686,399 of 3,686,400 bytes identical**.
- The only difference is byte offset 18 (pixel x=4, y=0, B component): `0x50` vs `0x90`.
- All 921,600 alpha values are identical between captures (8,447 pixels have alpha 255).
- Sample #7 texture identity `0xb400006f8202f248` was presented 4 times (presentation indices 127–130).
- Independent block-linear deswizzle yielded zero byte differences for all six guest/staging pairs (22,118,400 bytes checked).
- Sample #7 `ObserveUpload` was called on the `Sky-Present` thread. This does not prove whether the guest wrote invalid pixels, chose an uninitialized surface, or failed to update a valid display buffer.
- Other observed warnings: 287 unsupported extra layer-creation attempts, three `QueueBuffer` (`Free` instead of `Dequeued`) and three `CancelBuffer` state warnings, and one Turnip `VK_ERROR_OUT_OF_POOL_MEMORY`. These are not confirmed as causes.

The new queue-side instrumentation records `QueueBuffer`-to-texture-to-NvMap association and the upload-side guest mapping pointer. It does not change queue state, guest bytes, GPU barriers, or presentation order.


## Writer-provenance probe — November 6 follow-up

Current evidence: 381 distinct NvMap handles reduce to 13 nonoverlapping guest allocations; 380 presents correspond in order to 380 queue entries, and the recurring corrupted surface is guest VA `0x1817500000`. This does not identify its writer.

The diagnostic branch now records:

- `cpu_write_trap_hits` in the upload record: count of successful **post-trap CPU write faults** after a texture mapping has been registered. A fault occurs before the actual guest store and may cover many writes; zero does **not** exclude CPU writes made before the texture was first trapped.
- `last_gpu_write_serial` and `ever_used_as_gpu_writer` on upload, plus `gpu_texture_write_intent` records from `Texture::RecordGpuWrite()`. These track classification/intent, not GPU completion.
- `dma_write_intent` for mapped destinations overlapping pre-registered display surfaces in Maxwell DMA, inline-to-memory and mapped Maxwell engine paths. Source engine labels do not establish the final resource producer, and not every possible memory writer has been instrumented.
- `fence_count`, `first_fence_id`, `first_fence_threshold` at `QueueBuffer` (first active fence only). `PresentFrame()` still waits using the original fence logic; no artificial GPU waits or barriers were added.
- `RegisterDisplayBacking` during `SetPreallocatedBuffer` and `AttachBuffer`, before queue, limited to 24 physical mappings (maximum 8 MiB each). Watch matches compare host mapped pointer intervals, not guest-VA number alone.

Important limitations: guest CPU code may fill a buffer before it enters this watch, guest memory can be modified by paths not covered above, and an event labelled `*_intent` is **not** proof of completed GPU work. A known recurring buffer signature is not automatically a valid frame. The next ZIP will show which source *classes* operated on the destination during the observed window. A source-specific correction still requires a controlled experiment and a frame that is known to be correct.

Testing: install the new ARM64 DevRelease from PR #425, launch Dead Cells once, reproduce the opening glitches, exit to Strato's library, and long-press the existing logs icon to share `deadcells-probe.zip`. Do not reboot Dead Cells before export; probe data resets on launch.


## Third Moto G100 capture — candidate writer and address-relocation evidence

The latest probe ZIP contains 19 uploads, 311 queue operations, 310 presents, 96 early GPU texture writer intents, and **zero observed DMA/GPU buffer writer intents**. All 310 recorded presents agree with corresponding queued texture identities; the final queue was not presented before export.

Candidate upload **#7** (`0xb400006f8205a1c8`, guest VA `0x1817120000`) again has **8,447 opaque-alpha pixels** and the same colored-corruption layout. Relative to RenderDoc event #1380, precisely **3,686,398 / 3,686,400 staging bytes match**. The two differing bytes are at offsets 18 and 5,138. Relative to the immediately preceding Strato capture, those same two staging bytes differ; the guest swizzled inputs differ at offsets 34 and 50.

A decisive structural clue: the first bytes include a little-endian pointer **`0x1817120038`** at staging offset 16 — exactly guest VA **plus 0x38**. The preceding Strato capture has `0x1817500038` at that position, and RenderDoc has `0x1817900038`. Additional pointer-like metadata appears at staging offset 5,136. The otherwise near-identical bytes strongly resemble structured memory being displayed as a framebuffer, rather than a valid image. They do **not** yet establish who wrote those bytes or why this allocation is used for display.

For upload #7, `cpu_write_trap_hits=0`, `last_gpu_write_serial=0`, `ever_used_as_gpu_writer=false`. Zero traps cannot exclude earlier untrapped CPU writes; the 96-slot GPU write-intent stream was exhausted early, though zero serial directly indicates this texture was never classified a GPU writer. The absent DMA entries exclude only the specifically watched and hooked mappings.

Warnings in that run: 319 ignored extra display-layer creations, 5 `QueueBuffer` `Free`-instead-of-`Dequeued` warnings, 5 matching `CancelBuffer` warnings, and a Turnip `VK_ERROR_OUT_OF_POOL_MEMORY`. Treat these as separate hypotheses, not causal attribution.

### Targeted, read-only stage comparison

The diagnostic now calculates a **sparse 6 KiB fingerprint** of each 1280x720 display allocation when attached/preallocated, then samples the existing **guest mirror** in `PresentFrame()` after the original fence wait and before `SynchronizeHost()`. It also records the unaligned 64-bit header word at byte offset 16, where the relocatable guest pointer appeared. The log fields are `watch_matched`, `sparse_hash_on_registration`, `sparse_hash_after_fence`, `sparse_sample_changed`, and `header_u64_at16_*`.

A changed fingerprint demonstrates a change in sampled bytes between these two observation points, not which writer caused it. An unchanged fingerprint does not prove no changes outside those sectors. The initial registration probe reads guest-mapped CPU memory and may affect trap-protected behavior; compare performance/glitch behavior against the previous probe before interpreting results. No explicit GPU wait, barrier, or queue mutation was added.

Collect the next ZIP on the same driver and gameplay opening; only then decide whether to trace the allocator, an earlier CPU producer, or guest layer creation.


## Fourth capture: staging data predates display presentation (2026-11-06)

- New ZIP is intact: 426 queued surfaces, 426 matching present records, 20 texture uploads, 96 bounded GPU texture writer intent events. There are no recorded `dma_write_intent` or `gpu_buffer_write_intent` events; remember the watch begins only after surface attachment and the path coverage is incomplete.
- 229 of 426 presentation samples matched a registered display surface. The other 197 were unobserved (including many later surfaces beyond the 24-entry watch cap), **not** evidence of unchanged bytes.
- Exactly 7 matched presentations report a changed 6 KiB sparse hash, all for the same texture `0xb400006f82042488` (guest VA `0x1816900000`, indices 136–142). The sparse monitoring has positive controls.
- Candidate corrupted texture `0xb400006f8204e328`, guest VA `0x1817500000`, 8,447 opaque alpha pixels, was presented at indices **128–131** after queue fences (first syncpoint ID 1, thresholds 846/852/858/870). In all four presents, the sparse hash was `15315017125043354109` at both first surface attachment and after original fence wait. This is reproducible but excludes changes only in sampled bytes and only between those checkpoints.
- Recomputing `SparseHash(sample_7_guest.bin, 3686400)` independently gives exactly `15315017125043354109`, confirming the sampled data is the same guest layout as dumped by the staging upload path.
- The recurring structured self-reference is at **guest byte offset 0x20**: `0x1817500038` (`guest base + 0x38`). After block-linear deswizzle, the same 64-bit value is at **staging offset 0x10**. Earlier `header_u64_at16_*` fields measured guest offset 0x10, i.e. `0x100000001`, and therefore do **not** measure this pointer. The corrected probe separately records the guest 0x20 header.
- The first 64 bytes are structured, not typical RGBA frame bytes; there are also self-relative pointers in earlier captures. This is consistent with a non-image allocation being displayed, but does not prove which guest component wrote it, or whether the source memory was initialized as a valid render target.

### New early checkpoint

The next diagnostic build samples candidate 3,985,408-byte NvMap allocations in `NvMap::Alloc()` (max 24 distinct guest addresses, 6 KiB sparse sample each), saving `nvmap_alloc_snapshot` with the full guest address, allocation size, sample hash and guest offset 0x20 word. Compare against `present.sparse_hash_on_registration` and `present.sparse_hash_after_fence`. NvMap handle IDs are *not* unique allocation identities.

Reading the mapped allocation at `NvMap::Alloc` is diagnostic-only and can perturb guest page traps. It does not modify bytes or insert GPU waits. If no earlier snapshot exists, if allocation size differs, or if the watch cap is exhausted, do not infer that the data appeared later. The goal is to distinguish whether the structured bytes predate NvMap allocation, appear before binder attachment, or appear afterward.


## Fifth capture — the suspicious pixels predate NvMap allocation

Analyzed the attached `Diagnóstico.zip` with 24 `nvmap_alloc_snapshot` records, 19 uploads, 232 queued frames and 232 matching presentations. The target is upload **#7** at guest VA `0x1817500000`, NvMap handle 2884 on first queue, 8,447 opaque pixels. It was presented four times at indices 129–132.

The sparse FNV64 fingerprint of the guest memory is **15315017125043354109** at all of the following stages: immediately *after* `NvMap::Alloc`, when registered with the display surface, and after the original present fence wait. Independently hashing the saved `sample_7_guest.bin` with the identical sparse algorithm reproduces that same value. The little-endian guest field at offset `0x20` is **0x1817500038** (self-address + 0x38) at NvMap allocation, registration and presentation. The staging view contains that field at offset `0x10`, as expected for block-linear deswizzle. This is the earliest observed checkpoint: the guest gives NvMap a CPU VA that already contains these bytes; `NvMap::Alloc` is *not* necessarily the original guest memory allocation or the first writer.

The 24 snapshots include several other nonblank structures, including other guest self-references. The guest writes that populated these bytes may have occurred before NvMap allocation; no covered DMA/GPU buffer writers overlapped the later watch list. This does not prove either a game defect or an emulator defect. Separately, 224 presents matched one of the monitored surface allocations and 71 sampled presents indicated changes (two other textures); this shows the sparse hash can detect mutations but is not a byte-complete memory trace. The driver remained Turnip 24.0.99.

### Added prior-GPU-writer history (same PR #425)

The probe now retains only **2,048 most recent** mapped GPU writer intent records for guest host pointers with high address prefix `0x18xxxxxxxx`, including GPU image writers, buffer writer classifications, Maxwell DMA destination spans, and inline-to-memory destinations. During `NvMap::Alloc` it checks these earlier mapped spans for overlap and reports the latest source class, identity and event, plus the total number of retained matches and total history events seen. This is entirely observational: no new GPU wait, readback or image conversion. An absent match excludes only retained, mapped, covered GPU writers; untrapped CPU writes, stale GPU data not represented by these records, expired ring entries or missing aliases remain possible. A match proves only attempted writer overlap, not completed GPU publication.

### Immediate next diagnostic interpretation

If the suspect allocation records a prior GPU writer for the same physical backing, investigate lost GPU-to-guest publication / alias identity before changing any sync. If it records no prior writer with incomplete history, the absence is not decisive; a targeted guest CPU writer / allocation lifecycle trace may be needed. Do not implement a renderer workaround, blindly clear uninitialized surfaces, force full GPU waits, or merge this diagnostic PR without a reproduced causal mechanism.


## Broad correlated timeline checkpoint (2026-11-06) — diagnostic only

Earlier narrow probes captured valid guest snapshots but could not establish which resource/layer/producer was responsible. This single revision adds a bounded **multi-stage host timeline** to the existing `probe.jsonl`, while preserving the existing upload binaries. It is **not** a game fix, GPU readback completion probe, or evidence of a correct framebuffer.

New events, each containing a monotonically increasing `event_seq` and host `steady_clock` timestamp in microseconds:

- `timeline_layer`: actual create/open/close/destroy calls, producer identity, returned VI layer ID, and `reused` when another `CreateLayer` request maps to the same single Strato layer. Cap: 512 records.
- `timeline_slot`: dequeue, preallocate, queue, cancel and callback release, including producer identity, slot, original frame generation, state, handle and texture where available. `stale_release_callback` marks a callback whose captured frame generation no longer equals the slot's current generation. **It records the bug but intentionally does not change the slot release behavior**. Cap: 2,400.
- `timeline_texture`: selection of the texture on queue, with NvMap handle, cache texture identity, guest host mapping and `created_view_in_queue`. This flag only means the slot lacked a cached view, **not** that TextureManager created a new GPU image. Cap: 600.
- `timeline_nvmap`: creation, successful or failed allocation, and free of NvMap handles within the 2–16 MiB window; includes guest VA, handle, size. Cap: 1,000.
- `timeline_present_stage`: `before_fence`, `after_fence`, and `after_sync_host` on the real presentation path, with presentation-frame ID and texture pointer. These timestamp records are not Vulkan submission completion timestamps. Cap: 1,800.

Critical probe bug fixed: `ObservePriorWriter` previously filtered `reinterpret_cast<uintptr_t>(hostMapping)` by `(start >> 32) == 0x18`. This conflated the translated host pointer with the guest virtual address (the recurring guest addresses are 0x18...). The filter can silently discard valid preceding GPU/texture/buffer writers. It has been **removed**, while retaining overflow checks, a maximum event span of 256 MiB, and the 2,048-event circular history. Matching host pointer intervals still cannot identify aliases that map the same physical backing at different virtual addresses. A zero retrospective match is not proof of no GPU writer.

### What this one test should settle

1. Does the bad buffer belong to a reused/faked layer, and does another actual producer independently queue a different surface near it? If only the same producer queues all frames, duplicated `CreateLayer` warnings alone cannot prove omitted composition.
2. Are corrupted frames preceded by `stale_release_callback`, `CancelBuffer` of a non-dequeued slot, or a slot remapping? If not, do not attribute the bug to queue lifetime.
3. At the same host mapping, is there a GPU texture/buffer/DMA writer *intent* before NvMap allocation or queue? Retrospective overlap is a lead, never completion proof.
4. Does `after_fence` occur before `after_sync_host` and does the same texture identity connect queue, upload and presentation? Event serial and host timestamp give ordering, not GPU-write visibility.

### Safety and observability limits

The addition is metadata-only; it introduces **no new fence, no wait, no Vulkan operation and no buffer mutation**. Existing sampled guest reads and binary dumps from the old probe are unchanged. File logging is still synchronous on certain emulation thread paths and may perturb scheduling. Every subsystem has its own event limit to reduce starvation by noisy events; the emitted JSONL is finite. Identical host timestamps do not prove GPU completion. `created_view_in_queue` must not be interpreted as underlying GPU image creation. A stale callback report points to a real generation mismatch and existing release behavior, but is not itself proof that the Dead Cells corrupt frame was caused by that callback.

No merge or PR #400 change. One atomic update to PR Draft #425, with compiler result and APK status reported separately. Do not claim the build passed before it passes.


## Sixth capture — confirmed stale callback releases and limited fix (2026-11-06)

Source: CRC-valid `Diagnóstico.zip` from wide timeline HEAD a4527046. probe.jsonl contains 7,435 JSONL records: 564 queue, 563 present, 20 uploads and 24 NvMap snapshots. All timeline categories have individual caps; counts are lower bounds, not lifetime totals.

**Proven invalid transitions in the first 2,400 slot events:**
- 125 stale_release_callback events had a queued callback generation different from the current slot generation. 115 hit a slot still Queued, five Dequeued, five Free. In all 115 Queued cases the most recent observed queue entry belonged to a newer frame.
- Actual event sequence: event 283 queues generation 7 in slot 0; event 284 is a callback for generation 6, sees slot 0 Queued for generation 7 and nonetheless clears it; event 285 starts presenting generation 7. This is a real premature release of an in-flight frame.
- 110 ordinary release_callback observations, 108 while Queued and two while Free. The latter are not safe release events either.
- SetPreallocatedBuffer was invoked 1,453 times within the slot event cap; the old slot state was Queued on 12, Dequeued on 237. This path resets slots to Free and replaces backing even if in flight. **It is a separate potential lifecycle defect not fixed by the callback guard.**

**Layers:** 171 create, 171 open, 170 close events all reference the same single producer. Repeated layer creation warnings reflect rapid close/open cycles through one HOS Binder layer; they do not prove simultaneous layer composition was omitted. Queue/present texture identity matched for 563 consecutive pairs, leaving the last queue outstanding.

**Suspicious pixels:** upload #7, texture 0xb400006f82048888, guest VA 0x1817500000, 8,447 opaque pixels, GPU writer serial zero, guest trap count zero. Presented as frames 131–134. Its early NvMap sampled hash 15315017125043354109 and guest header word 0x1817500038 match earlier captures. The timeline includes preallocation while frame 131 was still Queued (event 3293) and a stale generation-132 callback releasing generation 133 while Queued (events 3321–3323). This is relevant queue disruption, **not proof of what originally wrote the structured guest bytes before NvMap allocation**.

**Limited corrective change in Draft PR #425:** use ShouldReleaseQueuedFrame(currentGeneration, completedGeneration, slotIsQueued) under the existing producer mutex. A stale or nonqueued callback now logs release_skipped_stale or release_skipped_not_queued, then exits without clearing another frame. Frame-bound release retains original notification. This follows the generation check in Eden's BufferQueueConsumer::ReleaseBuffer but does not introduce a new queue architecture or alter guest data or GPU synchronization.

**Validation:** standalone C++20 regression at tests/hosbinder_slot_release/run.py compiled and passed locally with Clang, ASan/UBSan, -Wall -Wextra -Werror and -pedantic. It directly reproduces the 283–285 event order. This is a helper-level contract test, not Android or actual gameplay validation. Avoid claiming that it fixes Dead Cells' pixel corruption until tested. The remaining SetPreallocatedBuffer overwrite behavior needs a separate causal design/proof. PR #400 remains untouched.


## Origin-focused memory/IOVA audit (2026-11-06) — diagnostic only

**Motivation from the post-1664a0e5 ZIP:** The previous broad BufferQueue capture shows that 128 stale callbacks were properly ignored. Its anomalous 1280x720 block-linear image is now upload #8 (guest VA `0x1817500000`), with 8,447 opaque alpha pixels and a persistent `0x1817500038` internal pointer. The sparse hash `15315017125043354109` is already present at `NvMap::Alloc()` and unchanged at the sampled presentation points. A zero `prior_writer_overlap_count` excludes only the recorded GPU-intent history, not CPU NCE stores, unobserved aliases or overwritten ring entries.

### Audit of real production paths

- In Strato, the graphics `soc::gm20b::GMMU` is a `FlatMemoryManager` of GPU IOVA -> **host pointers**. The separate `soc::SMMU` is for the host1x peripheral 32-bit address space; there is no independent GMMU->SMMU translation in the normal Maxwell graphics path. Tracking only an SMMU mapping would miss GPU resources.
- `AsGpu::MapBufferEx` fixed/dynamic, its remap path, sparse `Remap`, and `FreeMappingLocked` are concrete GMMU map/unmap boundaries. Each can associate an IOVA with a translated host interval. Their existence **does not** mean a GPU command wrote the interval.
- `NvMap::Handle::Alloc` associates a user-supplied address; it does not reserve or zero initial guest pixels. A self-referential pointer present at this point could be old heap content or CPU-provided metadata, not necessarily a damaged GPU transfer.
- Normal NCE ARM64 guest stores use direct mapped memory and may not pass `Buffer::MarkGpuDirty`, `Texture::RecordGpuWrite` or Maxwell DMA. Existing trap callbacks only observe accesses to already registered/trapped resources. We have **not** installed a new broad NCE write watch, so a missing writer event is never interpreted as 'CPU did not write'.
- Kernel memory `MapHeapMemory`, `MapPhysicalMemoryIfAllowed`, `UnmapMemory`, `FreeMemory` track guest mapping/release metadata. `SvcMapMemory` and `SvcUnmapMemory` contain actual host `memcpy` calls; record these as *completed kernel copies* after the copy returns. Ordinary heap maps are **not** CPU writes.
- `Buffer::SynchronizeGuest` performs a real `memcpy` from a GPU-buffer backing to guest-mirrored memory after waiting its fence; the additional `buffer_readback_published` records publication at this point. `Texture::CopyToGuest` encodes some pixel bytes into the guest mirror; `image_readback_encoded` records that an encode completed, **not** complete coverage of all padding or pixels.

### Targeted provenance implementation

- New pure C++ header `gpu/texture/deadcells_memory_origin_history.h`: finite 4,096-operation ring, paired host and guest range match, checked half-open interval overlap without address overflow, records IOVA and NvMap handle when available. Bounded by one extra per-subsystem mutex; no new page trap or GPU wait.
- New `ObserveMemoryOrigin` gathers only metadata into the ring at the above boundaries. The ring is independent from the file-logging mutex to avoid a lock inversion involving kernel memory mapping and the existing probe's guest-memory samples. No log IO occurs on those mapping paths.
- On each **first observed 3,985,408-byte NvMap allocation** (old candidate gate, maximum 24 distinct guest VAs), `DumpOriginHistoryLocked` writes at most 16 newest overlapping `memory_origin_event` entries and one `memory_origin_summary` with `matching_events_in_retained_history`, `matching_guest_intervals`, `matching_host_intervals`, `origin_history_overwritten`, plus `cpu_direct_stores_unobservable=true`. The sample runs **before** the NvMap allocation itself enters the history, so it cannot explain itself retroactively.
- The memory-origin history adds GMMU fixed/dynamic/remap/sparse-remap/unmap metadata, NvMap allocation/free history, kernel heap/physical memory mapping/unmap/release, kernel SVC copies, Inline2Memory IOVA write intents and actual Buffer/Image guest publications. The earlier DMA and texture writer-intent history is preserved.
- The new ring and output are **not exhaustive GPU write completion tracing** and cannot identify native CPU stores that precede the first NvMap association. Full guest CPU instruction tracing or page write trapping before an address is known would be considerably more invasive; it has not been added. Mirrored host pointers can alias the same physical backing without equality; match guest intervals where recorded and report missed aliases as a limit.

### Decision criteria for one subsequent targeted capture

1. **Previous GMMU alias:** a `gmmu_map_*` event with overlapping guest/host region and IOVA before the first bad NvMap association implies a prior GPU-accessible alias. Cross-check a genuine writer event or guest publication; mapping alone is not a producer.
2. **Kernel writeback:** a completed `svc_map_copy_dst`, `svc_unmap_copy_src`, `buffer_readback_published` or `image_readback_encoded` overlapping the target can identify a concrete earlier publication path; verify whether data changed and whether it was an owner of those bytes.
3. **Heap/reuse without observed writer:** repeated `heap_map` / `guest_unmap` / `nvmap_free` can show reuse of a guest interval. Without an observed writer, classify the origin as unobserved native CPU activity or preexisting bytes, **not** 'proven CPU writes' or 'uninitialized memory'.
4. **Negative result:** zero overlaps with `origin_history_overwritten > 0` is inconclusive. A zero count even with no overwrite still does not exclude direct guest CPU writes, uninstrumented GPU mappings or OS-level aliases. Do not add another generic BufferQueue timeline.

Native regression `tests/deadcells_memory_origin/run.py` compiles the exact origin-history header in C++20 with Clang, ASan and UBSan, covers overlapping/disjoint boundaries, alias dual matching, wrap/retention, empty records and reset. It is not a whole emulator build or a Dead Cells render test. The next Android compilation is not claimed successful before Actions confirms it.

No merge. Draft #425 remains the diagnostic branch. Draft #400 and its resource sequencing stays untouched.
