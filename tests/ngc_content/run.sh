#!/usr/bin/env bash
set -euo pipefail
repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -Wno-reorder -Wno-unused-parameter -Wno-missing-field-initializers -fsanitize=address,undefined -fno-omit-frame-pointer \
    -include functional -I"$repo_root/tests/ngc_content/host" \
    -I"$repo_root/tests/fssrv/host" -I"$repo_root/app/src/main/cpp/skyline" \
    "$repo_root/tests/ngc_content/content_tests.cpp" \
    "$repo_root/app/src/main/cpp/skyline/services/ngc/content_reader.cpp" \
    "$repo_root/app/src/main/cpp/skyline/vfs/rom_filesystem.cpp" \
    "$repo_root/app/src/main/cpp/skyline/vfs/os_filesystem.cpp" \
    "$repo_root/app/src/main/cpp/skyline/vfs/os_backing.cpp" \
    -o "$build_dir/tests"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$build_dir/tests" "$build_dir/fixture"
