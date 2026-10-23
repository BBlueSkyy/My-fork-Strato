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
existing zero-spacing layouts.

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
  This PR is intentionally Draft until a Release APK verifies the logo
  in-game. If the game still renders incorrectly, record the raw TIC
  value before drawing further conclusions.
- PR #386 mini-RenderDoc remains separate and is not merged.
