#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
build_dir="${TMPDIR:-/tmp}/strato-jit-tests"
mkdir -p "$build_dir"
"${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -Werror ${JIT_SANITIZER_FLAGS:-} \
 -I"$repo_root/app/src/main/cpp/skyline" \
 "$repo_root/tests/jit/plugin_tests.cpp" "$repo_root/app/src/main/cpp/skyline/services/jit/plugin_image.cpp" \
 -o "$build_dir/plugin_tests"
"$build_dir/plugin_tests"
if [[ -n "${DYNARMIC_BUILD_DIR:-}" ]]; then
 "${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -Werror \
  -I"$repo_root/app/src/main/cpp/skyline" -I"$repo_root/app/libraries/dynarmic/src" \
  "$repo_root/tests/jit/context_tests.cpp" \
  "$repo_root/app/src/main/cpp/skyline/services/jit/plugin_image.cpp" \
  "$repo_root/app/src/main/cpp/skyline/services/jit/plugin_context.cpp" \
  "$DYNARMIC_BUILD_DIR/src/dynarmic/libdynarmic.a" \
  "$DYNARMIC_BUILD_DIR/externals/fmt/libfmt.a" "$DYNARMIC_BUILD_DIR/externals/mcl/src/libmcl.a" \
  "$DYNARMIC_BUILD_DIR/externals/zydis/libZydis.a" "$DYNARMIC_BUILD_DIR/externals/zydis/zycore/libZycore.a" \
  -lpthread -o "$build_dir/context_tests"
 "$build_dir/context_tests"
fi

# Compile the production VMM and CodeMemory implementations, substituting only
# Android object/logging scaffolding and the fd factory for the Linux host.
sed 's/#include "types\/KProcess.h"/#include <kernel\/types\/KProcess.h>/' \
 "$repo_root/app/src/main/cpp/skyline/kernel/memory.cpp" > "$build_dir/memory.cpp"
sed 's/#include "KProcess.h"/#include <kernel\/types\/KProcess.h>/' \
 "$repo_root/app/src/main/cpp/skyline/kernel/types/KCodeMemory.cpp" > "$build_dir/KCodeMemory.cpp"
"${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -Wno-unknown-pragmas -Wno-reorder-init-list -Wno-invalid-constexpr -Wno-missing-field-initializers -Wno-unused-const-variable \
 -I"$repo_root/tests/jit/host" -I"$repo_root/app/src/main/cpp/skyline" \
 -I"$repo_root/app/src/main/cpp/skyline/kernel" -I"$repo_root/app/src/main/cpp/skyline/kernel/types" \
 -I"$repo_root/app/libraries/dynarmic/externals/fmt/include" \
 "$repo_root/tests/jit/code_memory_tests.cpp" "$build_dir/memory.cpp" "$build_dir/KCodeMemory.cpp" \
 "$repo_root/app/libraries/dynarmic/externals/fmt/src/format.cc" \
 -lpthread -o "$build_dir/code_memory_tests"
"$build_dir/code_memory_tests"

if [[ -n "${DYNARMIC_BUILD_DIR:-}" ]]; then
 "${CXX:-clang++}" -std=c++20 -O1 -g -Wno-reorder-init-list -Wno-invalid-constexpr \
  -I"$repo_root/tests/jit/host" -I"$repo_root/app/src/main/cpp/skyline" \
  -I"$repo_root/app/libraries/dynarmic/src" -I"$repo_root/app/libraries/dynarmic/externals/fmt/include" \
  -I"$repo_root/app/src/main/cpp/skyline/kernel" -I"$repo_root/app/src/main/cpp/skyline/kernel/types" \
  "$repo_root/tests/jit/service_tests.cpp" "$build_dir/memory.cpp" "$build_dir/KCodeMemory.cpp" \
  "$repo_root/app/src/main/cpp/skyline/services/jit/IJitService.cpp" \
  "$repo_root/app/src/main/cpp/skyline/services/jit/plugin_image.cpp" \
  "$repo_root/app/src/main/cpp/skyline/services/jit/plugin_context.cpp" \
  "$DYNARMIC_BUILD_DIR/src/dynarmic/libdynarmic.a" \
  "$DYNARMIC_BUILD_DIR/externals/fmt/libfmt.a" "$DYNARMIC_BUILD_DIR/externals/mcl/src/libmcl.a" \
  "$DYNARMIC_BUILD_DIR/externals/zydis/libZydis.a" "$DYNARMIC_BUILD_DIR/externals/zydis/zycore/libZycore.a" \
  -lcrypto -lpthread -o "$build_dir/service_tests"
 "$build_dir/service_tests"
fi
