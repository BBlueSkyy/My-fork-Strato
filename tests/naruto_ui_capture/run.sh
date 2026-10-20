#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -Werror -pthread \
  "$root/tests/naruto_ui_capture/test.cpp" -o "$binary"
"$binary"
echo "Naruto UI mini-RenderDoc budget/watch tests PASS"
