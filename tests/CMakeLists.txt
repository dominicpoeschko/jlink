cmake_minimum_required(VERSION 3.28)
project(jlink_tests CXX)

# jlink's host test against the in-process fake DLL: `cmake -S tests -B build -G Ninja && cmake --build build && ctest
# --test-dir build`. cmake_helpers comes by FetchContent (FETCHCONTENT_SOURCE_DIR_CMAKE_HELPERS points it at a local
# checkout).
include(FetchContent)

FetchContent_Declare(
    cmake_helpers
    GIT_REPOSITORY https://github.com/dominicpoeschko/cmake_helpers.git
    GIT_TAG master)

FetchContent_MakeAvailable(cmake_helpers)

include(${cmake_helpers_SOURCE_DIR}/BuildOptions.cmake)

set(JLINK_USE_FAKE_DLL
    ON
    CACHE BOOL "" FORCE)
add_subdirectory(../ ${CMAKE_CURRENT_BINARY_DIR}/jlink)
# the fake returns std::vector to the test: with USE_STDLIB_DEBUG both sides must agree on its layout
target_add_stdlib_debug_flags(jlink_fake_dll PRIVATE)
target_add_sanitizer_flags(jlink_fake_dll PRIVATE)

enable_testing()

add_executable(jlink_tests jlink_tests.cpp)
target_link_libraries(jlink_tests PRIVATE jlink::jlink)
target_compile_features(jlink_tests PRIVATE cxx_std_26)
target_add_default_build_options(jlink_tests PRIVATE)
# the test writes its RAM image's hex file into the working directory
add_test(
    NAME jlink_tests
    COMMAND jlink_tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(jlink_tests PROPERTIES TIMEOUT 60)
