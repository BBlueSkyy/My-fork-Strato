# NVDEC surface tests

Run `bash tests/nvdec/run.sh` with a C++20 compiler, pkg-config, FFmpeg CLI
(libx264/libvpx encoders), and libavcodec/libavutil/libswscale development files.
Use `NVDEC_SANITIZER_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'`
to enable sanitizers. `FFMPEG_CFLAGS` and `FFMPEG_LIBS` can override pkg-config.

The tests compile the production codecs, decoder, frame queue, surface writers
and GPU layout implementation. Host shims replace platform state and SMMU
storage, not decoding, conversion or tiling algorithms.

Coverage includes real H.264/VP9 decode without VIC, byte comparisons against
queued frames, independent streams, reused IOVAs, B-frame destination snapshots,
SPS cropping, bounded non-output submissions, NV12/NV24, picture-relative
offsets, woven/split fields, negative source strides, invalid/overlapping plane
ranges, and the existing VIC pitch/block-linear output.

The packet fixtures exercise the common `Codec` integration through a test
packet supplier. A production H.264 test also reads guest picture info and
registers and composes a packet containing a complete fixture bitstream.
VP8/VP9 header reconstruction is compiled but is not validated with captured
guest picture/entropy data. Field tests verify
plane row parity, not FFmpeg PAFF field-pair decoding. Decode-only AVFrames are written without entering the VIC queue. Invisible
VP9 reference buffers are retained through FFmpeg's public get_buffer2 callback
and materialized after synchronous decoding, without modifying show_frame or
reference state. Failed decode buffers are discarded. T210 NVDEC output is block-linear with two GOBs; newer NVDEC3 tiling
controls are not emulated. VIC retains its separately configured layouts.

Android device validation remains necessary: Grandia 1 must pass the previous
output-buffer timeout with correct video, and Dread, Link's Awakening and other
working videos must retain their NVDEC/VIC behavior.
