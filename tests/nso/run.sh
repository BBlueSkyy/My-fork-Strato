#!/usr/bin/env bash
set -euo pipefail
nso_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
nso_build=${1:-/tmp/strato-nso-tests}
# Boost.Regex 1.85 supports standalone header-only operation. Use an installed
# Boost tree or point this variable at boostorg/regex's include directory.
nso_boost=${NSO_BOOST_INCLUDE:-$nso_root/app/libraries/boost/libs/regex/include}
if [[ ! -f "$nso_boost/boost/regex/v5/regex.hpp" ]]; then
    echo 'Set NSO_BOOST_INCLUDE to Boost.Regex 1.85+ include directory' >&2
    exit 2
fi
mkdir -p "$nso_build/source"
# Compile exact production files. A symlinked include root substitutes only the
# platform-heavy Loader base class, with no production source extraction/copying.
for nso_file in nso.cpp nso.h zbic.cpp zbic.h; do
    ln -sf "$nso_root/app/src/main/cpp/skyline/loader/$nso_file" "$nso_build/source/$nso_file"
done
ln -sf "$nso_root/tests/nso/host/loader.h" "$nso_build/source/loader.h"
nso_sanitizers=()
if [[ ${NSO_SANITIZE:-0} == 1 ]]; then
    nso_sanitizers=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
cc -O0 -g "${nso_sanitizers[@]}" -I"$nso_root/app/libraries/lz4/lib" \
    -c "$nso_root/app/libraries/lz4/lib/lz4.c" -o "$nso_build/lz4.o"
cc -O0 -g "${nso_sanitizers[@]}" -I"$nso_root/app/libraries/zstd/lib" \
    -c "$nso_root/app/src/main/cpp/skyline/loader/zbic_zstd.c" -o "$nso_build/zbic_zstd.o"
c++ -std=c++20 -O0 -g -DFMT_HEADER_ONLY -DBOOST_REGEX_STANDALONE \
    "${nso_sanitizers[@]}" -I"$nso_build/source" -I"$nso_root/tests/nso/host" \
    -I"$nso_root/app/src/main/cpp/skyline" -I"$nso_root/app/libraries/fmt/include" \
    -I"$nso_root/app/libraries/lz4/lib" -I"$nso_root/app/libraries/zstd/lib" -I"$nso_boost" \
    "$nso_root/tests/nso/nso_tests.cpp" "$nso_build/source/nso.cpp" "$nso_build/source/zbic.cpp" \
    "$nso_build/lz4.o" "$nso_build/zbic_zstd.o" -o "$nso_build/nso_tests"
if [[ ${NSO_SANITIZE:-0} == 1 ]]; then
    # Avoid multi-GB malformed-size fixtures under ASAN's virtual-address mapping.
    for nso_case in raw lz4 zbic invalid_lz4 short_lz4 zero_compressed_size short_read header_short_read symbol_range_overflow ro_short_header ro_unterminated ro_invalid_length; do
        ASAN_OPTIONS=detect_leaks=0 "$nso_build/nso_tests" "$nso_case"
    done
else
    "$nso_build/nso_tests"
fi
