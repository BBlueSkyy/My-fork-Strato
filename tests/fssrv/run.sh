#!/usr/bin/env bash
set -euo pipefail

test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
test_build=${1:-/tmp/strato-fssrv-tests}

mkdir -p "$test_build"
g++ -std=c++20 -O1 -g \
    -I"$test_root/tests/fssrv/host" \
    -I"$test_root/app/src/main/cpp/skyline" \
    "$test_root/tests/fssrv/fssrv_tests.cpp" \
    "$test_root/app/src/main/cpp/skyline/vfs/os_filesystem.cpp" \
    "$test_root/app/src/main/cpp/skyline/vfs/os_backing.cpp" \
    -o "$test_build/fssrv_tests"
"$test_build/fssrv_tests"
