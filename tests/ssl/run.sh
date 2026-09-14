#!/usr/bin/env bash
set -euo pipefail

ssl_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
ssl_build=${1:-/tmp/strato-ssl-tests}
mkdir -p "$ssl_build"

make -C "$ssl_root/app/libraries/mbedtls/library" -j2 static

g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$ssl_root/tests/ssl/host" \
    -I"$ssl_root/app/src/main/cpp/skyline" \
    -I"$ssl_root/app/libraries/mbedtls/include" \
    "$ssl_root/tests/ssl/ssl_tests.cpp" \
    "$ssl_root/app/src/main/cpp/skyline/services/ssl/ISslService.cpp" \
    "$ssl_root/app/src/main/cpp/skyline/services/ssl/ISslContext.cpp" \
    "$ssl_root/app/src/main/cpp/skyline/services/ssl/ISslConnection.cpp" \
    "$ssl_root/app/src/main/cpp/skyline/services/ssl/state.cpp" \
    "$ssl_root/app/src/main/cpp/skyline/services/ssl/tls_backend.cpp" \
    "$ssl_root/app/libraries/mbedtls/library/libmbedtls.a" \
    "$ssl_root/app/libraries/mbedtls/library/libmbedx509.a" \
    "$ssl_root/app/libraries/mbedtls/library/libmbedcrypto.a" \
    -pthread -o "$ssl_build/ssl_tests"

ASAN_OPTIONS=detect_leaks=0 "$ssl_build/ssl_tests" "$ssl_root"
