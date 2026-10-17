#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir=${1:-/tmp/strato-nvhost-as-gpu-tests}
mkdir -p "$build_dir"
"${CXX:-c++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=undefined \
  -I "$root/app/src/main/cpp/skyline" \
  "$root/tests/nvhost_as_gpu/tests.cpp" -o "$build_dir/tests"
"$build_dir/tests"
echo 'nvhost-as-gpu fixed mapping lifecycle tests passed'
