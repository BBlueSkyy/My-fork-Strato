#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
set -euo pipefail
repo_root=$(cd "$(dirname "$0")/../.." && pwd)
test_build=$(mktemp -d)
trap 'rm -rf "$test_build"' EXIT
"${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -I"$repo_root/app/src/main/cpp/skyline" "$repo_root/tests/compute_diagnostics/tests.cpp" -o "$test_build/test"
ASAN_OPTIONS=detect_leaks=0 "$test_build/test" "$test_build/capture"

python3 "$repo_root/tests/compute_diagnostics/test_validate_capture.py"
