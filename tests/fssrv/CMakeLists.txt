cmake_minimum_required(VERSION 3.16)
project(strato_fssrv_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(STRATO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(SKYLINE "${STRATO_ROOT}/app/src/main/cpp/skyline")

add_executable(fssrv_tests fssrv_tests.cpp
    "${SKYLINE}/vfs/os_filesystem.cpp"
    "${SKYLINE}/vfs/os_backing.cpp")
target_include_directories(fssrv_tests PRIVATE host "${SKYLINE}")

enable_testing()
add_test(NAME fssrv COMMAND fssrv_tests)
