#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
build_dir="${TMPDIR:-/tmp}/strato-swkbd-tests"
mkdir -p "$build_dir"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror \
    -I"$repo_root/tests/swkbd/host" \
    -I"$repo_root/app/src/main/cpp" \
    "$repo_root/tests/swkbd/swkbd_tests.cpp" \
    "$repo_root/app/src/main/cpp/skyline/applet/swkbd/software_keyboard_config.cpp" \
    "$repo_root/app/src/main/cpp/skyline/applet/swkbd/software_keyboard_frontend.cpp" \
    "$repo_root/app/src/main/cpp/skyline/applet/swkbd/software_keyboard_state.cpp" \
    "$repo_root/app/src/main/cpp/skyline/applet/swkbd/software_keyboard_text.cpp" \
    -pthread -o "$build_dir/swkbd_tests"

"$build_dir/swkbd_tests"
