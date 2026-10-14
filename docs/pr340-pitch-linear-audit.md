# PR #340: pitch-linear compressed texture audit

Audited published HEAD: `013280e4065c6967da6caeb71ab20b111b9d7384`.
Base: `c7cb28d822c52437ed83e9f86ed15c3a3b77c6e7`.

All four original commits remain ancestors:

| Commit | Finding |
| --- | --- |
| `82a03ed9` | Block-row count is correct; raw copy pointers had no buffer extent or overflow checks. |
| `ddd46b47` | Both directions correctly select physical rows and visible row bytes; they needed bounded buffers. |
| `9ca3bef5` | `pitch * ceil(height / blockHeight)` is the correct padded 2D layer size, but its u32 conversion was unchecked. |
| `013280e4` | Existing tests passed locally with ASan/UBSan; coverage omitted short buffers, overflow, ASTC footprints and multiple layers. |

The user's device observations are accepted: menu and fights work, characters are visible, and the old intermittent crash apparently stopped. This audit does not reclassify those improvements as hypothetical. The new changes have host validation, not a new on-device gameplay run.

## Layout semantics

For width W, height H, block extent BW x BH, bytes per block B and guest byte pitch P:

- Physical columns = ceil(W / BW).
- `rowBytes` = physical columns * B.
- `rowCount` = ceil(H / BH).
- Packed staging layer = rowBytes * rowCount.
- Padded guest layer = P * rowCount.
- A copy accesses only (rowCount - 1) * P + rowBytes, not the final padding.
- Guest array layers use the explicit layer stride if supplied; it must cover a padded layer. Otherwise the calculated padded layer is the stride.

| Formats | Block extent | Bytes/block | Result |
| --- | --- | --- | --- |
| BC1, BC4 (including format variants) | 4 x 4 | 8 | Correct rounded columns and rows |
| BC2, BC3, BC5, BC6H, BC7 | 4 x 4 | 16 | Same row-count rule |
| ASTC | All 14 2D footprints, 4x4 through 12x12 | 16 | Independent width/height rounding, including asymmetric blocks |
| RGBA/BGRA | 1 x 1 | 4 | Pixel rows and unchanged component bytes |
| Other uncompressed formats | 1 x 1 | Format bpb | Pixel rows; no compressed-format division |

`FormatBase::GetSize()` already rounds both block axes. For supported 2D pitch textures its result equals the packed staging size; the original PR correctly uses `GetSize(width, 1)` for a physical block row. There is no reason to replace this with `width * bytesPerBlock` for BCn/ASTC.

The TIC reader reconstructs pitch using `PitchAlignmentBits` (5, i.e. 32-byte units). The CPU copy itself requires no new pitch rounding or pointer alignment: `memcpy` accepts unaligned byte addresses. Tests deliberately include small/unaligned padding as well as aligned pitches. Changing pitch to a rounded value would change the guest addresses. Layer size includes the full final pitch row, following the existing allocation contract.

Pitch helpers handle one 2D mip per layer. 3D pitch was explicitly unsupported in layout.h, and non-block mipmapped uploads already threw. These cases now fail before an incomplete mapping or copy instead of silently treating depth as one. Array-layer metadata/copying remains covered; this is not a claim that Switch hardware supports arbitrary pitch-linear TIC array configurations.

## Demonstrated defects and corrections

1. **Unbounded raw copies.** A 16-byte source with sourcePitch=16, rowBytes=8 and rowCount=3 produces an ASan stack-buffer-overflow in the original helper. There was no extent parameter with which it could detect the problem. Both wrappers now take spans, check source/destination extents before the first write, and receive actual per-layer lengths. Synchronization checks the complete mirror against the full pitch backing size before creating layer spans. This demonstrates a missing safety guarantee; it does not show that KOF supplied this exact short buffer.
2. **Layer-size truncation.** BC1 width=1, height=16384, pitch=0x100000 requires 4 GiB. The old cast to u32 returns zero. Checked arithmetic rejects layers that exceed GuestTexture's existing u32 stride range, without narrowing. Zero format block sizes/dimensions and pitches narrower than visible data also fail explicitly.
3. **Array mapping underestimation.** `GetSize()` multiplied two u32 operands before returning size_t: stride=0x80000000 and two layers returned zero instead of 4 GiB. It also subtracted baseArrayLayer even though synchronization copies layerCount backing layers. The pitch path now multiplies in checked size_t arithmetic and maps the complete backing. Base layer remains a view selection; no TextureManager matching/view logic is changed. Supplied layer strides and layer ranges are validated.
4. **Pointer extent and offset overflow.** The old row loop advanced pointers by a full pitch after the last row, potentially beyond even the one-past pointer when only visible final bytes existed. The new loop forms only validated visible-row offsets; it detects multiplication/addition overflow before pointer arithmetic.
5. **Odd-height BC array decoding.** The existing CPU fallback decoded a level as one image of height H * layers. For two 5x5 BC1 layers, separate layers contain eight blocks, but the tall-image interpretation consumes six and mixes layer boundaries. The old decoder call fails the independent RGB layer-color assertions. Pitch fallback now decodes each packed layer separately; native compressed GPU uploads are unchanged. The helper also bounds input/output layer extents. Host layer strides are size_t and now retain the complete size instead of an unnecessary u32 narrowing; pitch staging totals are checked before allocation.

These are generic metadata, bounds and conversion corrections. None changes emulated timing or speed-control policy, synchronization policy, waits, texture dirty tracking, GC, timeouts or game-specific behavior. Block-linear matching and the other named PRs are not imported. Remaining general block-linear/linear-only arithmetic and conversion issues are outside this pitch audit; those paths were not rewritten.

## Mature implementations and documentation

- [Yuzu util.cpp, archived commit 64e16a16](https://source.hodakov.me/hdkv/yuzu/src/commit/64e16a16291e10a8b9719bfd60ff9ce4417c7d32/src/video_core/texture_cache/util.cpp): CalculateGuestSizeInBytes uses pitch * ceil(height / format block height). This older snapshot predates the later compressed buffer-row-length correction; its old upload row length must not be used as a model.
- [Yuzu December 2023 report](https://yuzu-mirror.github.io/entry/yuzu-progress-report-dec-2023/): documents KOF XIII rendering being fixed by compressed buffer row length and BC CPU decoder changes. Its PR #12479 link now returns 404; this audit does not claim to have recovered that diff.
- [Eden mirror util.cpp](https://github.com/eden-emulator/mirror/blob/master/src/video_core/texture_cache/util.cpp): guest size uses the same rounded block rows. Pitch uploads retain guest pitch and convert bytes to texels using pitch * blockWidth / bytesPerBlock. Local source inspected, including UnswizzleImage and conversion paths.
- [Ryujinx SizeCalculator.cs](https://github.com/Stella-sea/ryujinx-admin/blob/canary/src/Ryujinx.Graphics.Texture/SizeCalculator.cs) and [LayoutConverter.cs](https://github.com/Stella-sea/ryujinx-admin/blob/canary/src/Ryujinx.Graphics.Texture/LayoutConverter.cs): downloaded and inspected archived source. Linear size is stride * ceil(height / blockHeight); upload conversion separately rounds width/height blocks. Host rows use 4-byte alignment, unlike Eden's retained pitch. The source also explicitly describes non-2D/mipmapped linear textures as unsupported by the Switch GPU. Not every historical conversion branch is suitable for copying into Strato.
- [NVIDIA public surface documentation](https://developer.nvidia.com/docs/drive/drive-os/7.2.5/public/drive-os-linux-sdk-api-ref-ea/structNvRmSurfaceRec.html): pitch is a byte offset between rows, and surface allocation size includes required padding. [Public address formula](https://developer.nvidia.com/docs/drive/drive-os/archives/6.0.3/linux/sdk/oxy_ex-1/common/topics/nvmedia_understand/NVM_SURF_ATTR_LAYOUTAttribute15.html) agrees with the row addressing used here. These are public NVIDIA surface references, not a recovered X1-specific compressed TIC specification. The [Tegra X1 TRM download](https://developer.nvidia.com/embedded/dlc/tegra-x1-technical-reference-manual) redirects to login, so a direct chapter-level X1 comparison was unavailable.
- [Vulkan VkBufferImageCopy](https://docs.vulkan.org/refpages/latest/refpages/source/VkBufferImageCopy.html): bufferRowLength/bufferImageHeight are texel counts; zero means tightly packed according to imageExtent. Strato strips guest pitch padding before upload and leaves both fields zero. Consequently, applying Yuzu's retained-pitch row-length patch to Strato would be wrong. Its packed BC/ASTC buffers are equivalent after block rounding without that change.

## Performance audit

The full PR diff touches row math/copying, calculated guest layer size and tests. The original PR adds no staging allocations, upload/download operations, BC/ASTC conversion pass, fence wait, deswizzle pass, shader work, resource invalidation or dirty-state transition.

Call paths inspected: SynchronizeHostImpl -> pitch upload wrapper; CopyToGuest -> pitch readback wrapper; CalculateLayerSize/GetLayerStride/GetSize -> guest mapping construction. SynchronizeHost/SynchronizeGuest dirty-state guards control when these run; they are unchanged. Upload staging allocation and optional BC deswizzle/decode storage existed before the PR. Readback staging reuse and waits are unchanged. ASTC remains native in this path; no ASTC CPU conversion was introduced.

For compressed data, the original PR decreases copied rows from H to ceil(H/BH), eliminating out-of-bounds extra copies. Uncompressed row loops perform the same number and size of copies. Pitch checks are per invocation, not per row; this audit's overflow and extent checks are also outside the row loop. The added BC layer iteration is only needed in the CPU fallback and avoids joining incomplete block rows; it introduces no additional buffer or recompression.

There is no on-device trace or profile from which to measure frequency, redundant reuploads or time spent per frame. No comparable playable pre-PR fight exists in the supplied evidence. Therefore neither a #340-caused 55–58 FPS regression nor an FPS gain from these safety fixes is demonstrated. Do not optimize sync, caching or allocation based on those numbers alone. The remaining FPS issue requires a separate measured runtime investigation; its cause cannot be assigned to this PR from this audit.

## Validation

Run `tests/gpu/run_pitch_linear_tests.sh` (GCC 13, C++20, ASan + UBSan):

- PASS: 310 independent layout round-trips, including both BC layout classes, every ASTC footprint, exact pitch, small/large padding, odd dimensions, 1/3 layers, layer gaps, wide/tall dimensions and RGBA/BGRA byte-copy semantics.
- PASS: original production GetLayerStride, CalculateLayerSize, GetSize and both pitch wrapper bodies, compiled through the existing repository host-facade/extraction approach. Covers round-trip/padding, short spans, invalid pitch/stride, base layer, u32 multiplication wrap, >u32 layer rejection and unsupported depth/mips. GPU allocation/mapping behavior is not simulated by this facade.
- PASS: 24 BC1/BC3 pitched upload + real production CPU decoder cases, with independently known red/green/blue layer colors, aligned and odd sizes. The decoder implementation is compiled unchanged; only unused platform-heavy logging headers are replaced for host compilation.
- Separate manual check, PASS: complete production layout.cpp compiled under ASan/UBSan against the existing NVdec host type facade (updated with mipLevelCount=1 for the new wrapper validation). This compilation is not part of the test runner above.
- PASS: `git diff --check`.

Negative checks: original raw helper fails ASan on a short source; original production GetSize fails the 4 GiB assertion; original merged-height BC call fails the per-layer RGB oracle. These are reproductions, not speculative fixes.

LeakSanitizer alone cannot read `/proc` tasks in this execution container; detect_leaks=0 avoids its fatal ptrace-environment error. AddressSanitizer and UndefinedBehaviorSanitizer stay enabled; test assertions stay enabled. No sanitizer reported an error in the passing runs. The test runner does not compile Android/Vulkan-dependent Texture allocation and synchronization as a full translation unit.

No Android SDK/NDK, Vulkan development headers or FFmpeg development/pkg-config toolchain is available locally. Full Android build, GPU/Vulkan validation and the full NVdec suite were not claimed. Publishing the follow-up commit triggers the existing `PR Build` pull_request/synchronize workflow (assembleDevRelease and assembleDevReldebug); do not monitor that build per the user's instruction. New-device verification should confirm the already-correct menu/fights/characters and crash behavior; no 60 FPS promise is made.
