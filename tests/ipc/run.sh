#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
build_dir="${TMPDIR:-/tmp}/strato-ipc-tests"
mkdir -p "$build_dir/kernel"
# Keep the production transport, replacing only Android process/session scaffolding.
sed -e 's/#include "types\/KProcess.h"/#include <kernel\/types\/KProcess.h>/' \
    -e 's/#include "types\/KSession.h"/#include <kernel\/types\/KSession.h>/' \
    "$repo_root/app/src/main/cpp/skyline/kernel/ipc.h" > "$build_dir/kernel/ipc.h"
sed -e 's/#include "ipc.h"/#include <kernel\/ipc.h>/' \
    -e 's/#include "types\/KProcess.h"/#include <kernel\/types\/KProcess.h>/' \
    "$repo_root/app/src/main/cpp/skyline/kernel/ipc.cpp" > "$build_dir/ipc.cpp"
g++ -std=c++20 -Wall -Wextra -I"$build_dir" -I"$repo_root/tests/ipc/host" \
    -I"$repo_root/app/src/main/cpp/skyline" "$repo_root/tests/ipc/ipc_tests.cpp" "$build_dir/ipc.cpp" \
    -o "$build_dir/ipc_tests"
"$build_dir/ipc_tests"
g++ -std=c++20 -Wall -Wextra -I"$repo_root/tests/ipc/host" -I"$repo_root/app/src/main/cpp/skyline" \
    "$repo_root/tests/ipc/callsite_tests.cpp" -o "$build_dir/callsite_tests"
"$build_dir/callsite_tests"
