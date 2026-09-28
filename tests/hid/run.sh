#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
build_dir="${TMPDIR:-/tmp}/strato-hid-tests"
mkdir -p "$build_dir"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror \
    -I"$repo_root/app/src/main/cpp" \
    "$repo_root/tests/hid/npad_assignment_tests.cpp" \
    -o "$build_dir/npad_assignment_tests"

"$build_dir/npad_assignment_tests"
