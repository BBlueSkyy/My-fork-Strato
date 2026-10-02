#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
build_dir="${TMPDIR:-/tmp}/strato-texture-tests"
mkdir -p "$build_dir"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror \
    -I"$repo_root/app/src/main/cpp" \
    "$repo_root/tests/texture/resource_compatibility_tests.cpp" \
    -o "$build_dir/resource_compatibility_tests"

"$build_dir/resource_compatibility_tests"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror \
    -I"$repo_root/app/src/main/cpp" \
    "$repo_root/tests/texture/guest_range_tests.cpp" \
    -o "$build_dir/guest_range_tests"

"$build_dir/guest_range_tests"
