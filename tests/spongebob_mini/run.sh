#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
c++ -std=c++20 -O1 -g -fsanitize=undefined -fno-sanitize-recover=all \
    -I app/src/main/cpp/skyline tests/spongebob_mini/tests.cpp -o "$OUT/sb-mini"
"$OUT/sb-mini"
python3 tests/spongebob_mini/test_decoder.py
echo "PASS: SpongeBob Mini controller and decoder"
