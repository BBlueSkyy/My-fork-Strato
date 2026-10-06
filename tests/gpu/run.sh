#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
out=${1:-/tmp/strato-gpu-mapping-tests}
mkdir -p "$out"
g++ -std=c++20 -O2 \
    -I"$root/tests/gpu/host" \
    -I"$root/app/src/main/cpp/skyline" \
    -I"$root/app/libraries/boost" \
    "$root/tests/gpu/buffer_mapping_tests.cpp" \
    -o "$out/buffer_mapping_tests"
"$out/buffer_mapping_tests"
