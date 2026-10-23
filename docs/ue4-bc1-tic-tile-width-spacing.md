# BC1 block-linear TIC tile-width-spacing — Demon Slayer Aniplex

## Evidence (captured on Android/Turnip)

The unchanged guest texture read back as a **1920x1080 BC1 sRGB**
Vulkan image in mini-RenderDoc PR #386 was already corrupted before the
logo composition draw. Three successive readbacks were byte-identical.

The corruption follows exactly one 128x256-pixel rectangle per
256-pixel-high ROB: x=0..127 in y=256..511,
x=128..255 in y=512..767, x=256..383 in y=768..1023,
and x=384..511 in the final partial ROB.

BC1 uses 8 bytes per 4x4 block, thus its 1920px logical row occupies
3840 bytes = 60 GOBs. At an 8-GOB block height, a ROB has 64 compressed
rows. Skyline previously advanced by 60*4096 = 245,760 bytes per ROB.
But the damaged rectangles correspond exactly to 4 missing GOB columns
(4*4096 = 16,384 bytes) at each 256 KiB boundary.

Removing 128 pixels from the *beginning* of each successive ROB in the
captured decoded image reassembled the Aniplex logo with no diagonal
noise. This **offline alignment reconstruction is not a GPU fix**;
it identifies the missing physical stride in the guest upload path.

## Root code divergence

Maxwell TIC exposes `sparseTileWidthGobsLog2` (known in yuzu as
`tile_width_spacing`). Strato parsed TIC Y and Z GOB dimensions, but
discarded its horizontal spacing. As a result guest memory range
calculation and block-linear deswizzling both assumed 60 physical
GOB columns for the Aniplex asset. For `tile_width_spacing=3`,
the physical pitch aligns to 64 GOB columns: 256 KiB per ROB,
even though the **visible width stays 1920px**.

This change carries the TIC spacing through `GuestTexture`, layer and
mipmap size calculations, and reversible swizzling. It skips the
additional physical GOB columns, preserving the logical width and all
existing zero-spacing single-level layouts.

Following yuzu, the extra spacing applies only when the image width is
larger than the requested alignment, its height spans more than one
vertical GOB block, and its depth covers the declared Z block.
Small textures retain the ordinary 64-byte GOB row alignment.

## Scope and validation

- No game-specific identifier, image patch, shader clamp, or viewport hack.
- Existing zero-spacing render-target and DMA call sites retain previous
  behavior.
- Native sampled-layout oracle includes BC1 1920x1080 with and without
  spacing and a small-size fallback.
- The actual Aniplex TIC field value was **not present in the capture**.
  The user subsequently confirmed that the original Release correction fixes
  graphics in several games. The audit follow-up below still needs device testing.
- PR #386 mini-RenderDoc remains separate and is not merged.

## Audit follow-up, 2026-10-23

The working 60-to-64-column correction is retained. Reference comparison used
[Eden/Yuzu layout helpers](https://github.com/eden-emulator/mirror/blob/master/src/video_core/texture_cache/util.cpp),
[archived Yuzu helpers](https://source.hodakov.me/hdkv/yuzu/src/commit/64e16a16291e10a8b9719bfd60ff9ce4417c7d32/src/video_core/texture_cache/util.cpp), and
Ryujinx's [SizeCalculator](https://github.com/fifv/ryujinx-mirror/blob/mirror/master/src/Ryujinx.Graphics.Texture/SizeCalculator.cs)
and [LayoutConverter](https://github.com/fifv/ryujinx-mirror/blob/mirror/master/src/Ryujinx.Graphics.Texture/LayoutConverter.cs).
The horizontal alignment and declared Y/Z eligibility follow Ryujinx's size
and copy paths; Yuzu/Eden independently include spacing in layer-end alignment.
This is a semantic adaptation; it does not copy their texture caches or decoders.

| Proven divergence | Minimal correction |
| --- | --- |
| Layer-end alignment omitted the X factor. BC1 1920x1080, BH8, spacing3, eight mips needs 1,900,544 bytes per array layer. | Include `2^spacing` in layer alignment. |
| Repeatedly ceil-halving GOB counts gives wrong odd BC/ASTC mip offsets. | Halve pixel dimensions first, then ceil to compression blocks and GOBs. This also repairs the pre-existing zero-spacing odd-mip defect. |
| Reducing Z blocks enabled spacing in mips whose depth is below the declared Z block. | Keep declared Y/Z thresholds, store only effective spacing per mip. |
| Materialization, mip-growth and mip-alias calculations defaulted to spacing zero. | Forward the stored spacing at those call sites. |
| A 960x270x4 RGBA8 volume with BH8/BD1, spacing3 advances Z planes by 1,310,720 bytes, but the slice helper used 1,228,800. | Use padded physical width for offsets and require matching parent/child ROB width. |
| Raw spacing equality split physically identical RT/TIC backings; changed spacing could also bypass alias reconciliation. | Compare physical widths at every mip for reuse; reconcile distinct supported encodings through the existing alias group. |

Focused validation: 20 independent address-oracle upload/readback cases, including
BC1, odd BC3/ASTC and 3D planes, preserve visible bytes and untouched padding;
four literal mip/array layout vectors; padded/unpadded slice origins; and actual
exact-overlap alias routing/lifecycle. ASan/UBSan passed. The complete layout
translation unit also passed host syntax compilation with the existing NVDEC
platform facade. Backing-promotion fixture declarations were synchronized;
the broader Vulkan suite was not run (its headers/runtime are unavailable here).

No per-pixel spacing checks, staging copies, logging or global synchronization
were added. Equal configurations retain the cheap equality path; zero-spacing
width calculation returns immediately. Equivalent RT/TIC encodings share a
backing instead of adding uploads/readbacks. These are code-level performance
observations, not an ARM64 FPS measurement.

**Draft / merge blocker:** the follow-up changes alias reuse and odd mip offsets,
so its ARM64 Release APK still needs Android rendering and performance acceptance.
No Android SDK/NDK build or device run was available locally. PR/master workflows
remain ARM64 Release only; Actions was not watched.
