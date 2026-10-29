#!/usr/bin/env bash
set -euo pipefail
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
test_build=${1:-/tmp/strato-npdm-tests}
mkdir -p "$test_build"
"${CXX:-g++}" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$test_root/tests/npdm/host" \
    -I"$test_root/app/src/main/cpp/skyline" \
    -I"$test_root/app/libraries/fmt/include" \
    "$test_root/tests/npdm/npdm_tests.cpp" \
    "$test_root/app/src/main/cpp/skyline/vfs/npdm.cpp" \
    -o "$test_build/npdm_tests"
ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0" "$test_build/npdm_tests"
