#!/usr/bin/env bash
set -euo pipefail
startup_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
startup_build=${1:-/tmp/strato-startup-tests}
mkdir -p "$startup_build"
"${CXX:-g++}" -std=c++20 -DFMT_HEADER_ONLY \
    -I"$startup_root/app/libraries/fmt/include" -I"$startup_root/app/src/main/cpp/skyline" \
    "$startup_root/tests/startup/trace_tests.cpp" -o "$startup_build/trace_tests"
"$startup_build/trace_tests"
echo 'PASS startup trace completion, exception unwind and incomplete scope'
