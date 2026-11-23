#!/usr/bin/env bash
set -euo pipefail
nvn_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
nvn_build=${1:-/tmp/strato-crysis-nvn-tests}
mkdir -p "$nvn_build"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror \
    -I"$nvn_root/app/src/main/cpp/skyline" \
    "$nvn_root/tests/crysis_nvn/tests.cpp" -o "$nvn_build/tests"
"$nvn_build/tests"
