# Dead Cells — same-session VkImage/guest/NvMap correlation (PR #425)

Scope: **diagnostic only**. This is NOT a graphical fix. Keep PR #425 Draft; do not merge; do not touch PR #400.

## Why this is necessary

The already-uploaded Strato `Diagnóstico.zip` / `probe.jsonl` (6,301 events) proves that guest surface `0x1817120000` (upload #7, frames 128–131, NvMap handles 2784/2804/2824/2848) contains a valid 2048x4096 PNG atlas at +0x4000, which is uploaded as a 1280x720 block-linear display image. The writer serial of that particular texture is zero.

The independent RenderDoc replay `renderdoc-ai-diagnostics.zip` shows **two different** 1280x720 Vulkan images: `0x6cc` is copied to the swapchain at event 1430, while `0x6d2` receives three draws at 1411/1416/1421 and appears as a Loading screen. The upload bytes into `0x6cc` match the Strato #7 staging within 2/3,686,400 bytes. **Neither ResourceId nor frame number can be carried between captures** without explicit same-session identity metadata.

The historical `strato-frame_frame364.rdc.strato-links.jsonl` includes TIC IOVAs and native image handles but **0 successful debug-name applications**. This is also from a separate capture. Do not join it to `0x6cc` or `0x6d2` from another RDC.

## New narrowly scoped observer

1. When supported, **optionally enable VK_EXT_debug_utils at instance creation** (no failure when absent).
2. `Texture::RecordGpuWrite()` observes 1280x720 GPU writer-classified images, naming each VkImage/host mapping pair only once per process for a bounded maximum of 256 pairs. A writer declaration is NOT a completed GPU command.
3. `GraphicBufferProducer::QueueBuffer()` names the exact 1280x720 VkImage selected by the guest GraphicBuffer, including the queue frame, NvMap handle, guest address, and guest-host pointer.
4. `PresentationEngine::PresentFrame()` names/logs the precise VkImage actually submitted to `Texture::CopyFrom()`, with the *presentation* frame ID.
5. `deadcells_probe::TraceVkImageLink` writes up to 4096 metadata-only `vulkan_image_link` JSONL events: role, native VkImage handle bits, Texture pointer, guest-host pointer, NvMap and guest VA when known, role-specific frame ID, optional label success, and up to four **unverified candidate** GMMU IOVAs from the existing bounded mapping history.
6. Neither a forced guest-memory read, GPU wait nor BufferQueue behavior is added. The existing GPU texture and buffer ownership policies remain unchanged.

**Warnings:** GMMU IOVA candidates are retrospective overlaps, NOT current page-table truth. An empty list does not prove missing mapping. Renaming an image can overwrite an earlier resource name; use the linked `vk_image_handle_bits` and per-role timeline to connect previous observations **only after an exact debug-name match inside the same boot**. Vulkan Image ResourceIds are not native Vulkan handles. A writer declaration is not GPU execution or an image being appropriate for the queued display frame.

## Offline checks

```sh
python3 tests/deadcells_display_sync/run_vk_identity.py
python3 tests/deadcells_display_sync/test_join_vk_resources.py

# After one same-boot capture and ZIP export:
python3 tests/deadcells_display_sync/join_vk_resources.py Diagnóstico.zip renderdoc-ai-diagnostics.zip > join_result.json
```

`join_vk_resources.py` never joins by approximate image size, hash, independently captured ResourceId, or process-local handle alone. It requires an **exact `DC425 ...` resource name** in RenderDoc `resources.jsonl` and `debug_name_applied=true` in that boot's Strato `probe.jsonl`. The old ZIP is expected to output `UNRESOLVED_NO_SAME_SESSION_IMAGE_LABELS`.

To test: run the diagnostic Strato APK on Moto G100 with the same video/intro segment, capture an RDC using the already-available RenderDoc Mobile, export AI diagnostics, then export Strato's diagnostic ZIP **without restarting the game**. If `VK_EXT_debug_utils` is unavailable, the JSONL should report `debug_name_applied=false`; do **not** assert a ResourceId association.

A positive exact-name join lets us compare which `VkImage` receives draws, which image `QueueBuffer` selects and which `PresentFrame` copies — plus host mapping identity and possible IOVA hints. It still does not prove the logical-frame relation without considering the queue/fence order. A future corrective patch must be separately justified.
