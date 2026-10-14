#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
c++ -std=c++20 -Wall -Wextra -Werror \
  -fsanitize=undefined -fno-sanitize-recover=all \
  -I"$root/app/src/main/cpp/skyline" \
  "$root/tests/gpu/texture_mapping_match_tests.cpp" \
  -o "$tmp/texture_mapping_match_tests"
"$tmp/texture_mapping_match_tests"
