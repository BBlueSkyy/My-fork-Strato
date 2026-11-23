#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir=${1:-/tmp/strato-homebrew-tests}
mkdir -p "$build_dir"
g++ -std=c++20 -O2 -Wall -Wextra -Werror \
    -I"$root/app/src/main/cpp/skyline" \
    "$root/tests/homebrew/nro_launch_path_tests.cpp" \
    -o "$build_dir/nro_launch_path_tests"
"$build_dir/nro_launch_path_tests"
