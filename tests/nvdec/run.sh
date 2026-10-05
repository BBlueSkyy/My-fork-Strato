#!/usr/bin/env bash
set -euo pipefail
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1}"
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
build_dir="${TMPDIR:-/tmp}/strato-nvdec-tests"
mkdir -p "$build_dir"
# Compile the production layout implementation with only its platform-heavy include
# replaced by host type declarations. No layout/conversion algorithm is mocked.
sed 's/#include "layout.h"/#include <gpu\/texture\/layout.h>/' \
 "$repo_root/app/src/main/cpp/skyline/gpu/texture/layout.cpp" > "$build_dir/layout.cpp"
read -r -a ffmpeg_flags <<< "${FFMPEG_CFLAGS:-$(pkg-config --cflags libavcodec libavutil libswscale)} ${FFMPEG_LIBS:-$(pkg-config --libs libavcodec libavutil libswscale)}"
"${CXX:-c++}" -std=c++20 -O1 -g ${NVDEC_SANITIZER_FLAGS:-} -Wall -Wextra -Wno-unknown-pragmas \
 -I"$repo_root/tests/nvdec/host" -I"$repo_root/app/src/main/cpp/skyline" \
 "$repo_root/tests/nvdec/surface_tests.cpp" \
 "$repo_root/app/src/main/cpp/skyline/soc/host1x/surface_writer.cpp" \
 "$repo_root/app/src/main/cpp/skyline/soc/host1x/classes/nvdec/output_surface.cpp" \
 "$repo_root/app/src/main/cpp/skyline/soc/host1x/classes/nvdec/surface_writer.cpp" \
 "$repo_root/app/src/main/cpp/skyline/soc/host1x/classes/nvdec/codecs/codec.cpp" \
 "$repo_root/app/src/main/cpp/skyline/soc/host1x/classes/nvdec/ffmpeg_decoder.cpp" \
 "$repo_root/app/src/main/cpp/skyline/soc/host1x/classes/vic/surface_writer.cpp" \
 "$repo_root/app/src/main/cpp/skyline/soc/host1x/classes/nvdec/codecs/h264.cpp" \
 "$repo_root/app/src/main/cpp/skyline/soc/host1x/classes/nvdec/codecs/vp8.cpp" \
 "$repo_root/app/src/main/cpp/skyline/soc/host1x/classes/nvdec/codecs/vp9.cpp" \
 "$repo_root/app/src/main/cpp/skyline/soc/host1x/frame_queue.cpp" \
 "$build_dir/layout.cpp" "${ffmpeg_flags[@]}" -o "$build_dir/surface_tests"
ffmpeg -v error -f lavfi -i 'testsrc2=size=64x48:rate=1' -frames:v 1 -c:v libx264 -bf 0 -tune zerolatency -threads 1 -pix_fmt yuv420p -f h264 -y "$build_dir/frame.h264"
ffmpeg -v error -f lavfi -i 'testsrc2=size=64x48:rate=1' -frames:v 1 -c:v libvpx-vp9 -threads 1 -pix_fmt yuv420p -f ivf -y "$build_dir/frame.ivf"
ffmpeg -v error -f lavfi -i 'testsrc2=size=64x48:rate=5' -frames:v 9 -c:v libx264 -bf 2 -threads 1 -pix_fmt yuv420p -f h264 -y "$build_dir/reorder.h264"
ffmpeg -v error -f lavfi -i 'testsrc2=size=64x50:rate=1' -frames:v 1 -c:v libx264 -bf 0 -tune zerolatency -threads 1 -pix_fmt yuv420p -f h264 -y "$build_dir/crop.h264"
"$build_dir/surface_tests" "$build_dir/frame.h264" "$build_dir/frame.ivf" "$build_dir/reorder.h264" "$build_dir/crop.h264"
