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
    "$repo_root/app/src/main/cpp/skyline/applet/swkbd/software_keyboard_inline.cpp" \
    "$repo_root/app/src/main/cpp/skyline/applet/swkbd/software_keyboard_state.cpp" \
    "$repo_root/app/src/main/cpp/skyline/applet/swkbd/software_keyboard_text.cpp" \
    -pthread -o "$build_dir/swkbd_tests"

"$build_dir/swkbd_tests"

# Compile the real applet and IApplet queues, stubbing only kernel/storage/JVM/logger edges.
applet_includes=(-I"$repo_root/tests/swkbd/applet_host" -I"$repo_root/tests/swkbd/host"
    -I"$repo_root/app/src/main/cpp" -I"$repo_root/app/src/main/cpp/skyline")
# IApplet.cpp already has an unused mode argument and initializer-order warning.
# Keep those exceptions limited to that unchanged translation unit.
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -Wno-reorder -Wno-unused-parameter \
    "${applet_includes[@]}" -c "$repo_root/app/src/main/cpp/skyline/services/am/applet/IApplet.cpp" \
    -o "$build_dir/swkbd_IApplet.o"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror \
    "${applet_includes[@]}" \
    "$repo_root/tests/swkbd/applet_protocol_tests.cpp" \
    "$repo_root/app/src/main/cpp/skyline/applet/swkbd/software_keyboard_applet.cpp" \
    "$build_dir/swkbd_IApplet.o" \
    "$repo_root/app/src/main/cpp/skyline/applet/swkbd/software_keyboard_config.cpp" \
    "$repo_root/app/src/main/cpp/skyline/applet/swkbd/software_keyboard_frontend.cpp" \
    "$repo_root/app/src/main/cpp/skyline/applet/swkbd/software_keyboard_inline.cpp" \
    "$repo_root/app/src/main/cpp/skyline/applet/swkbd/software_keyboard_state.cpp" \
    "$repo_root/app/src/main/cpp/skyline/applet/swkbd/software_keyboard_text.cpp" \
    -pthread -o "$build_dir/swkbd_applet_tests"
timeout 30 "$build_dir/swkbd_applet_tests"
