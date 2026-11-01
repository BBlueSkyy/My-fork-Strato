# PAC-MAN WORLD Re-PAC — third capture and intermittent HDR anomaly scan

Analysis date: 2026-11-01. The third uploaded `capture_001.zip`
(SHA256 `e07227f214ec7ef82f8b392e974bbdf0ebd1ef419272f2258eae17a94f3da688`)
was analyzed alongside `emulation.log`
(SHA256 `56ff1f7335adda9d71a0bce0ab3641711e011614fb65362b92b3925234302496`).

## What the eight raw GPU checkpoints actually show

Capture contains `capture_complete.txt`, `gpu_events.txt`,
`scene_00..07.txt` and `scene_00..07_*.raw`, plus
`snapshot_readbacks_done.txt`. Eight of eight raw image copies succeeded
(29,491,200 bytes), no lost events. All are
`B10G11R11UfloatPack32`, 1280×720.

Decode each little-endian 32-bit packed pixel as unsigned R11/G11/B10 float.
No NaN or infinity occurs in this capture.

| checkpoint | maximum finite RGB | pixels any RGB > 10 |
| --- | ---: | ---: |
| before scene draw 1 (ordinal 0) | 2.28125 | 0 |
| after 1 | 2.28125 | 0 |
| after 16 | 2.28125 | 0 |
| after 32 | 2.28125 | 0 |
| after 48 | 2.28125 | 0 |
| after 64 | 2.9375 | 0 |
| after 80 | 2.9375 | 0 |
| after 100 | 50 | 79 |

Only **298 of 921,600 pixels (0.0323%)** change between the
before-first-draw and after-first-draw images. The target already contains
a recognizable full previous scene and HUD before that draw. Between
draw 16 and 32, **710,452 pixels (77.09%)** change as scene
rendering updates the prior content. These are raw RT images; the raw
Y orientation is not the displayed screen orientation.

The prior *second* capture contained a giant bright/discontinuous region
already after draw 1 and 16, with 43,067 pixels above 10 and 10,541
pixels above 100; the third capture has zero pixels above 100 in
**all eight checkpoints**, even before the first draw. That is a true
inter-run difference in the GPU render target, not an absence of a
diagnostic file.

**Inference limited to evidence:** the scene target is reused and may
contain the previous scene before redraw. The capture with extreme
highlights and the capture without them differ from the very beginning
of their snapshots. This is compatible with intermittent corruption
originating in a previous frame, but does not prove missing clears,
aliasing, shader error, or Vulkan driver behavior. It also does not
prove the user-visible triangle was absent if no concurrent screen
recording was made.

The corresponding emulator log reports the eight completed image copies
and no GPU fault; it reports an unrelated audio minimum-latency error.

## Stage 4 opt-in HDR anomaly scan

In order to detect the *intermittent* high-intensity signature without
requiring a perfectly timed six-present capture, create this marker:

`gpu_capture/pacman_world/ENABLE_HDR_SCAN`

Then create/recreate the standard `gpu_capture/pacman_world/ARM_CAPTURE`
marker. HDR scan takes precedence over the stage-3
`ENABLE_SCENE_SNAPSHOTS` marker if both are present.

Only during this explicitly armed scan:
- Sample **after scene color draw 16**, where the previous good/bad
  runs clearly diverged.
- Continue for up to **60 host presentation intervals**, with at
  most 60 GPU readbacks and **4 in-flight buffers** (about 14 MiB).
- Decode 1280×720 `B10G11R11_UFLOAT_PACK32` pixels from
  GPU fence-complete staging buffers (not asynchronously reading
  unfinished GPU writes). Detect pixels with any channel >100 using
  exact R/G11/B10 packed-float thresholds. The anomaly criterion
  is **at least 512 such pixels**, far below the 10,541 seen in
  the bad capture but above the zero seen in this good capture.
- Persist **one baseline** and up to **four additional snapshots** on
  detection, at most five 3.7-MB RAW images in the folder.
  Scan-only frames are summarized in `hdr_scan.csv`, not saved as
  individual RAW files.
- `capture_complete.txt` only marks the end of collection.
  `hdr_scan_done.txt` confirms the GPU completion callbacks and
  reports requested/completed/saved/failed counts. Upload all
  `hdr_scan_*.raw/.txt`, both completion markers, the CSV,
  `gpu_events.txt` and the corresponding emulator log.

The scan only tests the **extreme-HDR signature** from the second
capture. A triangle without saturated HDR values will not trigger.
The diagnostic can perturb scheduling (one conditional renderpass split
per sampled interval), so non-reproduction during an armed scan does
not disprove a timing-dependent GPU coherence issue.

No rendering fix has been committed. Stage-4 APK/build and actual
device scan have **not** yet been validated. Keep Draft, no merge.
