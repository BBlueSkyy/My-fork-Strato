cmake_minimum_required(VERSION 3.16)
project(strato_xci_tests LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(STRATO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(SKYLINE "${STRATO_ROOT}/app/src/main/cpp/skyline")
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../vfs" vfs)
get_target_property(VFS_SOURCES vfs_tests SOURCES)
list(FILTER VFS_SOURCES EXCLUDE REGEX "storage_tests.cpp$")
add_executable(xci_tests xci_tests.cpp ${VFS_SOURCES}
    "${SKYLINE}/loader/xci.cpp" "${SKYLINE}/loader/nsp.cpp"
    "${SKYLINE}/vfs/rom_filesystem.cpp" "${SKYLINE}/vfs/ticket.cpp")
target_include_directories(xci_tests PRIVATE host ../vfs/host "${SKYLINE}"
    "${STRATO_ROOT}/app/libraries/fmt/include" "${STRATO_ROOT}/app/libraries/lz4/lib"
    "${STRATO_ROOT}/app/libraries/mbedtls/include")
target_compile_definitions(xci_tests PRIVATE FMT_HEADER_ONLY _GLIBCXX_ASSERTIONS)
target_link_libraries(xci_tests PRIVATE mbedcrypto)
enable_testing()
add_test(NAME xci_containers COMMAND xci_tests)
