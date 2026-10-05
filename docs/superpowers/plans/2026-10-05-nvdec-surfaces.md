# NVDEC decoded surfaces implementation plan

> **For agentic workers:** Use superpowers:executing-plans to implement these tasks inline.

**Goal:** Materialize decoded NVDEC surfaces in guest SMMU memory before making the same frame available to VIC.

**Architecture:** Snapshot output descriptors with each decode submission and carry a unique submission token through FFmpeg PTS. Share plane layout writing with VIC; retain its presentation queue and stream isolation. Model T210 output rather than interpreting newer NVDEC3 fields as a pitch selector.

**Tech Stack:** C++20, FFmpeg, existing SMMU and block-linear helpers, Android Gradle/NDK.

**Spec:** User request in this session; diagnostic evidence PR #303.

## Global constraints

- Base: master 4619f3ebc84adfce5ffa18ec5eedb56a257a5551.
- No game conditions, artificial waits/fences, multi-program/JIT32/GPU changes or diagnostic logs.
- Preserve VIC handoff and independent streams; leave #303 and master untouched.
- Publish a dedicated PR and Android test build; no merge.

## Review focus

- Decoder reordering or repeated IOVAs must retain each submission's pitches/chroma/offsets.
- T210 zero tiling fields mean block-linear output, not pitch.
- Invalid indices, addresses, strides and unsupported source formats must not corrupt memory.
- Interlaced field planes must receive alternate rows, not duplicate progressive images.
- Buffered VP9 output must use the buffered picture's destinations.

### Task 1: Shared plane writer and H.264 surface descriptors

**Files:** host1x/surface_writer.*, nvdec/output_surface.*, VIC surface_writer.cpp, tests/nvdec/*.

**Interfaces:** OutputSurface snapshots plane addresses, pitches, dimensions and field layout; WriteDecodedSurface consumes that snapshot and an AVFrame. WriteSurfacePlane shares existing layout/SMMU helpers with VIC.

- [x] Write and run failing host tests for byte-accurate NV12/NV24, pitches, offsets, fields and invalid inputs.
- [x] Implement the generic writer and H.264 descriptor using currPicIdx and picture-relative offsets.
- [x] Reuse plane writing from VIC; verify its conversion and layout remain compatible.
- [x] Run host tests and diff checks.

### Task 2: Decode submission integration

**Files:** nvdec/codecs/codec.*, h264.*, vp8.*, vp9.*, vp9_types.h, ffmpeg_decoder.*.

**Interfaces:** Codec snapshots output descriptors; unique FFmpeg token resolves the originating submission and its existing presentation key.

- [x] Add failing integration tests with real H.264/VP9 decoding, repeated addresses and independent streams.
- [x] Materialize received H.264/VP8/VP9 frames and then retain their AVFrame for VIC.
- [x] Keep VP9 descriptors alongside its buffered picture; preserve hidden-frame presentation eligibility.
- [x] Run tests; inspect failures before changing behavior.

### Task 3: Verification and publication

- [x] Run relevant host tests and git diff --check.
- [x] Review the complete diff against the spec and references.
- [ ] Build Android (CI if local SDK/Gradle unavailable), inspect results and repair build failures.
- [ ] Open dedicated PR with test evidence and explicit pending device regressions.
- [ ] Provide PR/build links; do not claim device gameplay tests were run here.

## References and decisions

- NVIDIA open-gpu-doc/classes/video/nvdec_drv.h: addresses are in units of 256; output_memory_layout selects NV12/NV24; configurable gob_height starts at NVDEC3.
- averne/FFmpeg nvtegra/libavutil/hwcontext_nvtegra.c: T210 decode uses two GOBs vertically; VIC input block-height log2 is 1. Its H.264 driver fills tileFormat/gob_height with zero.
- alula/Ryujinx qlaunch/src/Ryujinx.Graphics.Nvdec/H264Decoder.cs: writes decoded surfaces immediately; interlaced writer splits alternate plane rows. Its fixed block-height differs from the hardware-backed reference and is not copied.
- eden-emulator/mirror: picture-relative frame/field offsets and per-submission output metadata; current software path retains a frame queue.
- Status/frameStats: hardware status includes decode/error counters, but no evidence here establishes a required completion marker. Do not invent counters or write status merely because a buffer is configured.
