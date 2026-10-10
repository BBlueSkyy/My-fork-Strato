#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
build_dir="${TMPDIR:-/tmp}/strato-texture-tests"
mkdir -p "$build_dir"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror \
    -I"$repo_root/app/src/main/cpp" \
    "$repo_root/tests/texture/resource_compatibility_tests.cpp" \
    -o "$build_dir/resource_compatibility_tests"

"$build_dir/resource_compatibility_tests"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror \
    -I"$repo_root/app/src/main/cpp" \
    "$repo_root/tests/texture/guest_range_tests.cpp" \
    -o "$build_dir/guest_range_tests"

"$build_dir/guest_range_tests"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror \
    -I"$repo_root/app/src/main/cpp" \
    "$repo_root/tests/texture/resource_layout_tests.cpp" \
    -o "$build_dir/resource_layout_tests"

"$build_dir/resource_layout_tests"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror \
    -I"$repo_root/app/src/main/cpp" \
    "$repo_root/tests/texture/copy_dependency_tests.cpp" \
    -o "$build_dir/copy_dependency_tests"

"$build_dir/copy_dependency_tests"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror \
    -I"$repo_root/app/src/main/cpp" \
    "$repo_root/tests/texture/copy_capability_tests.cpp" \
    -o "$build_dir/copy_capability_tests"

"$build_dir/copy_capability_tests"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror \
    -DSKYLINE_TEXTURE_STORAGE_METADATA_ONLY \
    -I"$repo_root/app/src/main/cpp" \
    -I"$repo_root/app/libraries/vkhpp" \
    -I"$repo_root/app/libraries/vkhpp/Vulkan-Headers/include" \
    "$repo_root/tests/texture/storage_capability_tests.cpp" \
    -o "$build_dir/storage_capability_tests"

"$build_dir/storage_capability_tests"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror \
    -I"$repo_root/app/src/main/cpp/skyline" \
    "$repo_root/tests/texture/copy_only_runtime_serialization_tests.cpp" \
    -o "$build_dir/copy_only_runtime_serialization_tests"

"$build_dir/copy_only_runtime_serialization_tests"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror \
    -I"$repo_root/app/src/main/cpp/skyline" \
    "$repo_root/tests/texture/maintenance5_support_tests.cpp" \
    -o "$build_dir/maintenance5_support_tests"

"$build_dir/maintenance5_support_tests"
