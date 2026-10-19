#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
out=${1:-/tmp/strato-network-policy-tests}
mkdir -p "$out"
c++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$root/app/src/main/cpp/skyline" \
    "$root/tests/network_offline/network_policy_tests.cpp" -o "$out/network_policy_tests"
"$out/network_policy_tests"
