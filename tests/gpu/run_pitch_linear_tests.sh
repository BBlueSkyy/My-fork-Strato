#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
set -euo pipefail
repo_root=$(cd "$(dirname "$0")/../.." && pwd)
test_build=$(mktemp -d)
trap 'rm -rf "$test_build"' EXIT
compiler=${CXX:-g++}
flags=(-std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
include=(-I"$repo_root/app/src/main/cpp/skyline")
# LeakSanitizer cannot inspect /proc tasks in some container/ptrace environments.
# ASan and UBSan stay enabled; these tests use automatic storage and RAII vectors.
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0"
"$compiler" "${flags[@]}" -Wall -Wextra -Werror "${include[@]}" \
    "$repo_root/tests/gpu/pitch_linear_tests.cpp" -o "$test_build/pitch-tests"
"$test_build/pitch-tests"
# Use the existing GPU host-test approach: extract exact production method
# bodies, with no independent implementation of their arithmetic or copy loop.
python3 - "$repo_root" "$test_build" <<'PYTHON'
import pathlib, sys
root, build = map(pathlib.Path, sys.argv[1:])
def function(text, signature):
    start = text.index(signature)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]
gpu = root / "app/src/main/cpp/skyline/gpu/texture"
texture = (gpu / "texture.cpp").read_text()
layout = (gpu / "layout.cpp").read_text()
(build / "pitch-production.inc").write_text(
    "namespace skyline::gpu {\n" + "\n".join(function(texture, signature) for signature in
        ("u32 GuestTexture::GetLayerStride(", "u32 GuestTexture::CalculateLayerSize(", "size_t GuestTexture::GetSize(")) +
    "\n}\nnamespace skyline::gpu::texture {\n" + "\n".join(function(layout, signature) for signature in
        ("void CopyPitchLinearToLinear(", "void CopyLinearToPitchLinear(")) + "\n}\n")
PYTHON
"$compiler" "${flags[@]}" -Wall -Wextra -Werror -Wno-return-type "${include[@]}" -I"$test_build" \
    "$repo_root/tests/gpu/pitch_linear_integration_tests.cpp" -o "$test_build/integration-tests"
"$test_build/integration-tests"
# Compile the actual decoder with its implementation unchanged. Only replace
# Android exception/logging headers, which are unused with decoder asserts off.
mkdir -p "$test_build/host/fmt"
cat > "$test_build/host/common.h" <<'CPP'
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
CPP
printf '#pragma once\n' > "$test_build/host/fmt/printf.h"
"$compiler" "${flags[@]}" -DNDEBUG -w -I"$test_build/host" \
    -c "$repo_root/app/src/main/cpp/skyline/gpu/texture/bc_decoder.cpp" -o "$test_build/bc-decoder.o"
# Test assertions remain enabled (NDEBUG applies only to the decoder object).
"$compiler" "${flags[@]}" -Wall -Wextra -Werror "${include[@]}" \
    "$repo_root/tests/gpu/pitch_linear_bc_tests.cpp" "$test_build/bc-decoder.o" -o "$test_build/bc-tests"
"$test_build/bc-tests"
