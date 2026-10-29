#!/usr/bin/env bash
set -euo pipefail
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
test_build=${1:-/tmp/strato-xci-tests}
mkdir -p "$test_build"
make -C "$test_root/app/libraries/mbedtls/library" -j4 libmbedcrypto.a > "$test_build/mbedtls.log" 2>&1
gcc -O1 -c "$test_root/app/libraries/lz4/lib/lz4.c" -o "$test_build/lz4.o"
g++ -std=c++20 -O1 -g -DFMT_HEADER_ONLY -D_GLIBCXX_ASSERTIONS \
    -I"$test_root/tests/xci/host" -I"$test_root/tests/vfs/host" -I"$test_root/app/src/main/cpp/skyline" \
    -I"$test_root/app/libraries/fmt/include" -I"$test_root/app/libraries/lz4/lib" \
    -I"$test_root/app/libraries/mbedtls/include" \
    "$test_root/tests/xci/xci_tests.cpp" \
    "$test_root/app/src/main/cpp/skyline/vfs/"{bktr,nca,cnmt,nacp,ctr_encrypted_backing,sparse_storage,compressed_storage,partition_filesystem,rom_filesystem,ticket}.cpp \
    "$test_root/app/src/main/cpp/skyline/loader/"{program_content,resolve_content,xci,nsp}.cpp \
    "$test_root/app/src/main/cpp/skyline/crypto/aes_cipher.cpp" \
    "$test_build/lz4.o" "$test_root/app/libraries/mbedtls/library/libmbedcrypto.a" \
    -pthread -o "$test_build/xci_tests"
"$test_build/xci_tests" "${@:2}"
