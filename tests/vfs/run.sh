#!/usr/bin/env bash
set -euo pipefail
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
test_build=${1:-/tmp/strato-vfs-tests}
mkdir -p "$test_build"
make -C "$test_root/app/libraries/mbedtls/library" -j4 libmbedcrypto.a > "$test_build/mbedtls.log" 2>&1
gcc -O1 -c "$test_root/app/libraries/lz4/lib/lz4.c" -o "$test_build/lz4.o"
g++ -std=c++20 -O1 -g -DFMT_HEADER_ONLY \
    -I"$test_root/tests/vfs/host" -I"$test_root/app/src/main/cpp/skyline" \
    -I"$test_root/app/libraries/fmt/include" -I"$test_root/app/libraries/lz4/lib" \
    -I"$test_root/app/libraries/mbedtls/include" \
    "$test_root/tests/vfs/storage_tests.cpp" \
    "$test_root/app/src/main/cpp/skyline/vfs/"{bktr,nca,cnmt,ctr_encrypted_backing,sparse_storage,compressed_storage,partition_filesystem}.cpp \
    "$test_root/app/src/main/cpp/skyline/loader/program_content.cpp" \
    "$test_root/app/src/main/cpp/skyline/loader/resolve_content.cpp" \
    "$test_root/app/src/main/cpp/skyline/crypto/aes_cipher.cpp" \
    "$test_build/lz4.o" "$test_root/app/libraries/mbedtls/library/libmbedcrypto.a" \
    -pthread -o "$test_build/vfs_tests"
"$test_build/vfs_tests"
