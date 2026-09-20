#!/usr/bin/env bash
set -euo pipefail

test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
test_build=${1:-/tmp/strato-fssrv-tests}

mkdir -p "$test_build"
g++ -std=c++20 -O1 -g \
    -I"$test_root/tests/fssrv/host" \
    -I"$test_root/app/src/main/cpp/skyline" \
    "$test_root/tests/fssrv/fssrv_tests.cpp" \
    "$test_root/app/src/main/cpp/skyline/vfs/os_filesystem.cpp" \
    "$test_root/app/src/main/cpp/skyline/vfs/os_backing.cpp" \
    "$test_root/app/src/main/cpp/skyline/services/fssrv/IFile.cpp" \
    "$test_root/app/src/main/cpp/skyline/services/fssrv/IStorage.cpp" \
    "$test_root/app/src/main/cpp/skyline/services/fssrv/IFileSystem.cpp" \
    "$test_root/app/src/main/cpp/skyline/services/fssrv/IDirectory.cpp" \
    "$test_root/app/src/main/cpp/skyline/services/fssrv/ISaveDataInfoReader.cpp" \
    "$test_root/app/src/main/cpp/skyline/services/fssrv/IMultiCommitManager.cpp" \
    -o "$test_build/fssrv_tests"
"$test_build/fssrv_tests"

g++ -std=c++20 -fsyntax-only \
    -I"$test_root/tests/fssrv/host" \
    -I"$test_root/app/src/main/cpp/skyline" \
    "$test_root/app/src/main/cpp/skyline/services/fssrv/IFileSystem.cpp" \
    "$test_root/app/src/main/cpp/skyline/services/fssrv/IDirectory.cpp" \
    "$test_root/app/src/main/cpp/skyline/services/fssrv/IFile.cpp" \
    "$test_root/app/src/main/cpp/skyline/services/fssrv/IStorage.cpp" \
    "$test_root/app/src/main/cpp/skyline/services/fssrv/ISaveDataInfoReader.cpp" \
    "$test_root/app/src/main/cpp/skyline/services/fssrv/IMultiCommitManager.cpp" \
    "$test_root/app/src/main/cpp/skyline/services/fssrv/IFileSystemProxy.cpp"
