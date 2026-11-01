# PAC-MAN WORLD Re-PAC — capture 002: HDR scene pixel evidence

Analysis date: 2026-11-01. This is the **second** file uploaded as
`capture_001.zip` (not the metadata-only first capture). No prior
capture folder was overwritten in the repository.

Capture ZIP SHA256:
`dea8133e3485fc8573036d41684e1eb328e33d4bd8a3f9ffa98f33fc375b074a`

Corresponding `emulation.log` SHA256:
`40d359f4ef4fe1ab778ea35eb80ede0a00fdccdf62ce37ff3c1ebada52839a92`

## Verified files and execution

- `snapshot_mode=1`, `recorded_presentations=6`, `events_written=1188`,
  `dropped_events=0`.
- `snapshot_readbacks_done.txt`: requested=6, written=6, failed=0,
  bytes_reserved=22,118,400. Each image is 3,686,400 bytes
  (1280 × 720 × 4).
- All six snapshots share the same scene backing texture pointer
  `0xb400006f05854588`, format
  `VK_FORMAT_B10G11R11_UFLOAT_PACK32`, `General` layout.
- Interval 1: after scene draw ordinals 1, 16, 64. Interval 2:
  same ordinals. No after-draw-176 snapshot exists.
- The contemporaneous log confirms Qualcomm proprietary 0.777.0
  on Adreno 650 and an absent external RenderDoc layer. The log
  reports all six image writes.

## Independent raw pixel decode

Read little-endian uint32 pixels. Decode the first 11 bits as
unsigned R11 float (5-bit exponent, 6-bit mantissa), next 11 as
G11 and top 10 as B10 (5-bit exponent, 5-bit mantissa).
Handle subnormals, infinity and NaN explicitly. No sRGB transfer
or synthetic RGBA interpretation is used in measurements.

| Snapshot | Interval | After draw | pixels any RGB >10 | fraction of frame | pixels >100 | nonfinite pixels |
|---|---:|---:|---:|---:|---:|---:|
| scene_00 | 1 | 1 | 43,067 | 4.673% | 10,541 | 0 |
| scene_01 | 1 | 16 | 43,067 | 4.673% | 10,541 | 0 |
| scene_02 | 1 | 64 | 12,261 | 1.330% | 2,653 | 0 |
| scene_03 | 2 | 1 | 43,068 | 4.673% | 10,548 | 0 |
| scene_04 | 2 | 16 | 43,068 | 4.673% | 10,548 | 0 |
| scene_05 | 2 | 64 | 12,277 | 1.332% | 2,657 | 0 |

The maximum decoded finite channel value is 11,264 in all images
(in the first interval at image position x=697, y=677). This is
an unusually large finite HDR value; it does **not** identify the
shader that generated it or prove the triangle is caused by HDR.

- After draw 1, the scene is already recognizably complete, including
  the inverted *raw* HUD; do not assume the raw render-target orientation
  is the display orientation.
- The bright triangular/discontinuous region is already visible
  in the earliest image. Drawing it in draw 1 is **not proven**:
  the RT may contain previous-frame content.
- Between draw 1 and draw 16, 10.46% of raw pixels change. Between
  16 and 64, 73.00% change (72.94% in the second interval),
  including replacement of much of the bright region.
- The two intervals are very close but not byte-identical:
  interval 1 vs 2 after draw 1 shares 94.14% identical packed pixels;
  after draw 64 shares 97.59%.

## Required stage 3 and how it changes the hypothesis

A *pre-first-scene-draw* GPU readback is necessary to distinguish
pre-existing/stale image contents from a newly drawn triangle.
Also sample after ordinals 1, 16, 32, 48, 64, 80 and 100.
Only the first complete scene interval is sampled (8 × 3.69 MB),
so the eight-image quota is not exhausted before the last scene pass.

Why this choice:
1. The six successful snapshots prove the image-copy path works;
   no need to redesign the mini-RenderDoc.
2. This capture contains only **101 eligible 1280x720 color draws
   per interval**. The former ordinal-176 probe was impossible.
3. Values disappear/are overwritten between 16 and 64, but that
   identifies an *overwriting* operation, not necessarily the
   origin of the artifact.
4. Content persistence across intervals is compatible with normal
   RT reuse. It is not itself proof of a missing guest clear or of
   a TextureManager aliasing error.

The next capture is still timing-perturbing (diagnostic renderpass
splits and readbacks) and still not a complete Vulkan `.rdc`.
No shader/rasterizer/Texman fix should be applied without
localizing the first operation that produces the **wrong** pixel.
