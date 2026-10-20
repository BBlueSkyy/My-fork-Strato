#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
c++ -std=c++20 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$root/app/src/main/cpp/skyline" \
    "$root/tests/gpu/tic_cache_v2_tests.cpp" \
    -o "$tmp/tic_cache_v2_tests"
"$tmp/tic_cache_v2_tests"
