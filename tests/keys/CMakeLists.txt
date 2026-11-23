cmake_minimum_required(VERSION 3.16)
project(strato_key_store_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(STRATO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(SKYLINE "${STRATO_ROOT}/app/src/main/cpp/skyline")

# utils.h also contains Android properties and ARM counter assembly. Compile its
# actual hex conversion helpers, without those unrelated platform dependencies.
file(READ "${SKYLINE}/common/utils.h" UTIL_SOURCE)
string(FIND "${UTIL_SOURCE}" "    constexpr u8 HexDigitToNibble" HEX_START)
string(FIND "${UTIL_SOURCE}" "    template<typename Type>\n    requires std::is_integral_v<Type>\n    constexpr Type HexStringToInt" HEX_END)
if(HEX_START LESS 0 OR HEX_END LESS HEX_START)
    message(FATAL_ERROR "Cannot locate production hex conversion helpers")
endif()
math(EXPR HEX_LENGTH "${HEX_END} - ${HEX_START}")
string(SUBSTRING "${UTIL_SOURCE}" ${HEX_START} ${HEX_LENGTH} HEX_HELPERS)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/hex_conversion.h"
    "#pragma once\nnamespace skyline::util {\n${HEX_HELPERS}\n}\n")

add_executable(key_store_tests key_store_tests.cpp
    "${SKYLINE}/crypto/key_store.cpp"
    "${SKYLINE}/vfs/os_filesystem.cpp"
    "${SKYLINE}/vfs/os_backing.cpp")
target_include_directories(key_store_tests PRIVATE host "${CMAKE_CURRENT_BINARY_DIR}" "${SKYLINE}")
enable_testing()
add_test(NAME key_store COMMAND key_store_tests)
