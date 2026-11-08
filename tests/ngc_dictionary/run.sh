#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
bin="$(mktemp /tmp/strato-ngc-dictionary.XXXXXX)"
fixtures="$(mktemp -d /tmp/strato-ngc-fixtures.XXXXXX)"
python3 tests/ngc_dictionary/make_fixtures.py "$fixtures"
trap 'rm -f "$bin"; rm -rf "$fixtures"' EXIT
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -Iapp/src/main/cpp tests/ngc_dictionary/tests.cpp app/src/main/cpp/skyline/services/ngc/ngc_dictionary.cpp -lz -o "$bin"
"$bin" "$fixtures"
