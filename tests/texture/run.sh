#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
set -euo pipefail
repo_root=$(cd "$(dirname "$0")/../.." && pwd)
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0"
for test in guest_range mapping_cache; do
    "${CXX:-c++}" -std=c++20 -O1 -g -Wall -Wextra -Werror \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I"$repo_root/app/src/main/cpp" \
        "$repo_root/tests/texture/${test}_tests.cpp" -o "$build_dir/$test"
    "$build_dir/$test"
    echo "PASS: $test"
done
