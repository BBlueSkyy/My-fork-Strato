# Cosmic Shake cubemap provenance — phase 7

Status: diagnostic only. The cause of the phase-6 corrupt-looking cube is **not proven**.
No functional renderer fix, new corrective PR, HDR clamp, shader workaround, or workflow change.

## What static analysis establishes

The phase-6 evidence is the [on-device report](https://github.com/BBlueSkyy/My-fork-Strato/pull/375#issuecomment-6315694684), not a guest-memory capture. It links fragment `7161B14045F710B5` to a 128x128, six-face, eight-mip FP16 backing and establishes the draw-20 HDR amplification. It does not identify the original TIC format or the first boundary at which its bytes become wrong.

- `Textures::GetTexture` takes the guest format from the TIC. `ConvertHostCompatibleFormat` can produce an FP16 backing from BC6H when native BC6H is unsupported; a native FP16 guest also stays FP16. These alternatives cannot be distinguished by the existing backing readback.
- `FormatBase::IsCompatible` does not admit arbitrary compressed/uncompressed format reuse. `Texture::GetView` substitutes the converted host format when the requested format equals the guest format. Thus the presence of an FP16 view alone does not demonstrate a compressed-byte reinterpretation bug.
- Cube backings are 2D images with six array layers and `eCubeCompatible`. `GetBufferImageCopies` describes every mip with all six layers, tightly packed in mip-major order. `SynchronizeHostImpl` rearranges guest layer-major mip chains into that same order. The existing readback uses these same regions. No missing-four-faces upload command was found in this path.
- A correct deswizzle is a byte permutation/copy; it cannot turn a valid FP16 alpha into `0xAAAA`, but wrong addresses, stale mappings, or wrong format interpretation can select such bytes. Successful BC6H decoder output uses alpha `0x3C00`; this observation is not proof that the suspect guest texture is BC6H, nor that every texel passed through a successful decode.
- Upload barriers already cover initialized images and scope visibility to the entire mip/layer range. Alias groups retain writer history and can materialize converted representations read-only. Static inspection alone cannot establish which alias/history the suspect image actually used.

## Comparison with an inspectable Eden/Yuzu lineage

Reference: Eden Xbox tree at commit [`33969e85769916535d02f124705ff7aa02086775`](https://github.com/juanresendiz813/eden-xbox/tree/33969e85769916535d02f124705ff7aa02086775), which retains Eden/Yuzu attribution. This is a reference snapshot, not a claim about the current canonical Eden release.

| Path | Strato PR #375 | Reference | Relevance to this capture |
| --- | --- | --- | --- |
| Cube storage vs view | `layerCount=6`, `baseArrayLayer=TIC.BaseLayer()`; guest size subtracts base layer | `image_info.cpp` reserves `BaseLayer()+6`; `image_view_info.cpp` separately describes six sampled layers | Concrete difference for nonzero base layers; phase 6 does not show the TIC base layer. Do not apply a speculative base-layer fix. |
| Initial block geometry | Sampled Y clamp is limited to single-mip surfaces; multi-mip layout starts with declared Y GOB count | `util.cpp::AdjustMipBlockSize` reduces Y for the actual mip extent, including mip zero | A multi-mip TIC with oversized declared block height can diverge. Actual guest format and block geometry are missing. |
| Packing/upload | Guest is layer-major; host is mip-major/all layers, with one copy per mip | `util.cpp::UnswizzleImage` also emits all layers of each mip, then advances the mip offset | Same packing semantics for the inspected ordinary 2D cube case; not itself evidence of an upload bug. |
| GPU virtual remaps | GMMU updates mappings; cached TIC views can return without a new translation, including across channel sequence updates when the TIC is unchanged | `texture_cache.h::UnmapGPUMemory` handles remapped images; `UnmapMemory` unregisters/deletes affected images | Concrete invalidation difference. No phase-6 evidence proves the suspect IOVA was remapped. |
| BC fallback | Software decode only when guest/host formats differ; block-linear arrays concatenate layer heights | `decode_bc.cpp::DecompressBlocks` also concatenates height and layer count | Concatenation is **not** a demonstrated difference here. No BC6H change is justified until the original format is known. |

## Minimal additional evidence

Place an empty `gpu_capture/ue4/ENABLE_UPLOAD_PROVENANCE` file in Strato's public application files **before starting the game**, then fully restart the game/app. The switch is read once by `GPU::Initialise`. In gameplay with the corruption visible, use the existing `gpu_capture/ue4/ARM_CAPTURE` trigger. Remove the enable file and restart to disable retention.

Enabling before launch is necessary: the cube may have been uploaded long before manual frame capture. Reading its current guest mirror or forcing a reupload at draw 20 would not reconstruct the original upload and could conceal the bug.

While enabled, retain the latest real CPU upload for block-linear, six-layer, depth-one FP16 host images, regardless of whether the guest is FP16 or converted BC6H. No title ID or replacement shader is used. The three retained blobs are:

- `_guest.raw`: actual mapped guest bytes at upload time, layer-major, including padding.
- `_linear.raw`: bytes after the actual production deswizzle, before optional format conversion; mip-major/all layers.
- `_upload.raw`: bytes actually supplied to `copyBufferToImage`, after any production conversion; mip-major/all layers.

The existing draw-20 material readback supplies the GPU side. Provenance mode suppresses the already-completed generic HDR and other-material focused readbacks; it retains only eligible FP16 cube readbacks plus the existing presentation/session artifacts. It does not introduce another GPU copy or GPU completion wait.

The matching `focus_NN_provenance.txt` records guest/host formats, tiling, stride, upload mapping spans, guest/linear/host offsets and CRC32 for every mip/face, upload-vs-readback exact equality and first differing byte. `CUBE_TIC`/`CUBE_TIC_WORD` events record the cached TIC; `CUBE_GMMU_CURRENT` events translate its IOVA again without reading guest bytes or changing cache decisions. For aliased views, compare view offsets before treating backing-extent spans as identical logical ranges.

Retention is off by default, capped at 16 MiB of live blob bytes across images/callbacks, with per-image limits of 4 MiB guest plus 2 MiB linear plus 2 MiB host. Metadata is additional. The budget releases with snapshot lifetime; replacing a last upload never silently reuses an older proof if the new reservation fails. A missing proof is reported, not interpreted as a clean upload. Only armed readback callbacks write these files.

## Interpretation and limits

This compares the **last real upload** with the **draw-time GPU readback**, not an isolated immediate post-upload readback. The manifest records actual GPU-writer history, backing generation, dirty state, freed memory and alias state. If an image has GPU writes, a changed generation or an alias authority change, different bytes do not prove an upload failure. A recorded copy plus completed readback fence still cannot prove that guest bytes were valid by game semantics.

1. Establish the TIC guest format and mapping correspondence first.
2. If guest and deswizzled bytes already contain the abnormal encodings with matching correct addresses, investigate the producer/history rather than Vulkan conversion.
3. If guest addresses are correct but deswizzled pixels differ from an independent address oracle, investigate geometry/stride/mip addressing.
4. Investigate BC6H only if the original guest format is BC6H. Compare the saved predecode blocks and actual staging output before changing the decoder.
5. If staging differs from readback, rule out intervening GPU writes, backing replacement and alias authority changes before attributing it to upload or driver behavior.
6. If current IOVA mappings differ from upload-time mappings, establish the remap timing and whether the cached view is stale.

## Local verification

- Existing sampled-layout/alias fixtures: ASan/UBSan; independent address oracle now covers all eight mips and six faces at 128x128 for native 8-byte pixels and 16-byte 4x4 blocks. This validates those layouts, not the actual game input or BC6H decoding.
- Existing mini-capture fixture: retention opt-in, hard byte cap/release, CRC known vector, per-face equality/mismatch and invalid-range handling. Its old 48-claim assertion was updated to the existing 64-image quota.
- Existing submission fixture: UBSan; upload proof selection occurs after attached texture uploads and before the recording-thread handoff, and runs once.
- `git diff --check`.

No Android SDK/NDK build or on-device Vulkan validation was performed locally. Still required: compile the APK, launch with retention enabled, capture the same corrupt scene once, and compare the new provenance blobs/manifest. Keep #375 Draft; no merge and no automatic Actions monitoring.


## Phase 8 — explicit reason for missing upload proof

The first correctly armed provenance capture reported `provenance_enabled=true`,
`proof_available=false`, with a current FP16 guest/host cube (six layers,
eight mips, base layer 0) and no observed GPU writes or aliases. The
cubemap readback happened to contain only zero bytes, while the final frame
remained excessively bright. That result **does not establish** where the
texture was initialized or whether the readback matches the actual shader
sampling event.

The previous code wrote only a generic list of reasons when proof retention
failed. This revision records the actual result for each cubemap's most
recent `Texture::SynchronizeHostImpl` attempt:

- `never_observed`: no eligible CPU synchronization attempted for this
  texture while tracing was enabled;
- `synchronize_entered`: synchronization began but did not reach proof
  retention (for example, an exception);
- `no_staging_buffer`, `unsupported_tile_mode`,
  `guest_bytes_over_limit`, `linear_bytes_over_limit`,
  `upload_bytes_over_limit`: the exact disqualifying condition;
- `budget_rejected`: limits were otherwise satisfied, but the 16 MiB
  global retention budget declined the reservation;
- `proof_retained`: the immutable upload inputs were retained;
- `upload_recorded`: those retained bytes were actually submitted
  to `copyBufferToImage`.

Each `focus_XX_provenance.txt` includes `proof_reason`,
`sync_attempts`, `copy_buffer_to_image_count`,
`proof_discard_count`, `budget_used_on_reject`,
`budget_used_now` and the existing 16 MiB cap, plus guest tile mode,
guest mirror bytes, stride, linear bytes and upload bytes. The reason is
reported even when `proof_available=false`; a copy count can distinguish
an upload performed without retained proof from a never-uploaded texture.
Counters and the proof-selection state are updated only when
`ENABLE_UPLOAD_PROVENANCE` was enabled before boot.

This is an observability-only change. No upload is forced, no wait/barrier
or texture decoding is modified, and no GPU-copy quota is raised. Repeat
the same fully restarted opt-in capture once and send the matching ZIP/log.


## Phase 9 — avoid starving the draw-20 cube retention budget

The on-device phase-8 capture produced `proof_reason=budget_rejected`,
`sync_attempts=1`, `copy_buffer_to_image_count=1`,
`budget_used_on_reject=16766784` and `budget_limit=16777216` for
the sampled 128x128, six-face, eight-mip FP16 cubemap. The last real upload
was recorded by Vulkan, but there were only 10,432 bytes left and retaining
its mapped guest, linear and staged bytes would need approximately 3.18 MiB.
This is a **confirmed instrumentation budget failure, not a proven graphics
root cause**. That same readback contained NaN/extreme FP16 patterns again.

When `ENABLE_UPLOAD_PROVENANCE` is set, keep lightweight status/copy counters
for all six-layer FP16 cubemaps, but allow full byte retention **only** for
native guest+host FP16 backings with the observed target geometry: 128x128,
depth 1, six layers, eight mipmaps. Other cubemaps receive
`proof_reason=outside_focused_cubemap`, avoiding reservation contention.
The existing 16 MiB global budget and per-image limits remain unchanged.

This does not guarantee an available proof if more than a few identically
sized cubemaps are uploaded before the target, or if the renderer selects
a different backing. If budget rejection recurs, do **not** increase the
limit blindly: use the actual new diagnostics to distinguish that case.

Restart with `ENABLE_UPLOAD_PROVENANCE` present **before game startup**,
arm the same corrupt scene, and send the whole ZIP and matching log.
The normal renderer/upload path is unchanged, and this remains a Draft
diagnostic only.
