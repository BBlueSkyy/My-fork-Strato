cmake_minimum_required(VERSION 3.16)
project(strato_vfs_tests LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(STRATO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(SKYLINE "${STRATO_ROOT}/app/src/main/cpp/skyline")
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
add_subdirectory("${STRATO_ROOT}/app/libraries/mbedtls" mbedtls EXCLUDE_FROM_ALL)
add_executable(vfs_tests storage_tests.cpp
    "${SKYLINE}/vfs/bktr.cpp"
    "${SKYLINE}/vfs/nca.cpp"
    "${SKYLINE}/vfs/cnmt.cpp"
    "${SKYLINE}/loader/program_content.cpp"
    "${SKYLINE}/loader/resolve_content.cpp"
    "${SKYLINE}/vfs/ctr_encrypted_backing.cpp"
    "${SKYLINE}/vfs/sparse_storage.cpp"
    "${SKYLINE}/vfs/compressed_storage.cpp"
    "${SKYLINE}/vfs/partition_filesystem.cpp"
    "${SKYLINE}/crypto/aes_cipher.cpp"
    "${STRATO_ROOT}/app/libraries/lz4/lib/lz4.c")
target_include_directories(vfs_tests PRIVATE host "${SKYLINE}"
    "${STRATO_ROOT}/app/libraries/fmt/include" "${STRATO_ROOT}/app/libraries/lz4/lib")
target_compile_definitions(vfs_tests PRIVATE FMT_HEADER_ONLY)
target_link_libraries(vfs_tests PRIVATE mbedcrypto)
enable_testing()
add_test(NAME storage_layers COMMAND vfs_tests)
