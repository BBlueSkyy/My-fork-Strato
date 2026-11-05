#!/usr/bin/env bash
set -euo pipefail
ngc_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
ngc_build=${1:-/tmp/strato-ngc-tests}
mkdir -p "$ngc_build"
python3 "$ngc_root/tests/ngc_dictionary/make_fixtures.py" "$ngc_build/fixtures"
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$ngc_root/tests/ngc/host" -I"$ngc_root/app/src/main/cpp/skyline" \
    "$ngc_root/tests/ngc/ngc_tests.cpp" \
    "$ngc_root/app/src/main/cpp/skyline/services/ngc/INgcServiceForApplication.cpp" \
    "$ngc_root/app/src/main/cpp/skyline/services/ngc/ngc_dictionary.cpp" \
    "$ngc_root/tests/ngc/content_reader_stub.cpp" \
    -pthread -lz -o "$ngc_build/ngc_tests"
ASAN_OPTIONS=detect_leaks=0 "$ngc_build/ngc_tests" "$ngc_build/fixtures"
