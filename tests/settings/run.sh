#!/usr/bin/env bash
set -euo pipefail
settings_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
settings_build=${1:-/tmp/strato-settings-tests}
mkdir -p "$settings_build"
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$settings_root/tests/settings/host" -I"$settings_root/app/src/main/cpp/skyline" \
    "$settings_root/tests/settings/settings_tests.cpp" \
    "$settings_root/app/src/main/cpp/skyline/services/settings/settings_store.cpp" \
    -pthread -o "$settings_build/settings_tests"
"$settings_build/settings_tests"
