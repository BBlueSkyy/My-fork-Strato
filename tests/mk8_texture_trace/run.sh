#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
"${CXX:-g++}" -std=c++20 -O2 -pthread -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I app/src/main/cpp/skyline tests/mk8_texture_trace/tests.cpp -o "$test_dir/tests"
ASAN_OPTIONS=detect_leaks=0 "$test_dir/tests"
echo 'PASS bounded MK8 recorder: disabled, phase isolation, limits, overflow, export, concurrency'
