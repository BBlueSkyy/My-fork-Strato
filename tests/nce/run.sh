#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
build_dir="${TMPDIR:-/tmp}/strato-nce-tests"
mkdir -p "$build_dir"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror \
    -I"$repo_root/tests/nce/include" -I"$repo_root/app/src/main/cpp/skyline" \
    "$repo_root/tests/nce/context_layout_tests.cpp" -o "$build_dir/context_layout_tests"
"$build_dir/context_layout_tests"
"${PYTHON:-python3}" "$repo_root/tests/nce/context_tests.py"
