#!/usr/bin/env bash
set -euo pipefail
host1x_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
host1x_build=$(mktemp -d /tmp/strato-host1x-XXXXXX)
trap 'rm -rf "$host1x_build"' EXIT
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$host1x_root/tests/host1x/host" -I"$host1x_root/app/src/main/cpp/skyline" \
    "$host1x_root/tests/host1x/tests.cpp" \
    "$host1x_root/app/src/main/cpp/skyline/soc/host1x/syncpoint.cpp" \
    -pthread -o "$host1x_build/tests"
ASAN_OPTIONS="${ASAN_OPTIONS:-}:detect_leaks=0" "$host1x_build/tests"
