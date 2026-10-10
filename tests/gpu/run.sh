#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
build_dir="${TMPDIR:-/tmp}/strato-gpu-tests"
mkdir -p "$build_dir"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror \
    -I"$repo_root/app/src/main/cpp" \
    -I"$repo_root/app/src/main/cpp/skyline" \
    "$repo_root/tests/gpu/usage_tracker_tests.cpp" \
    -o "$build_dir/usage_tracker_tests"

"$build_dir/usage_tracker_tests"
