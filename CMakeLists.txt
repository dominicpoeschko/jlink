cmake_minimum_required(VERSION 3.18)

project(jlink)

find_library(libjlinkarm jlinkarm REQUIRED HINTS /opt/SEGGER/JLink)

find_package(fmt 8 QUIET)
if(NOT fmt_FOUND)
  FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG master)
  FetchContent_MakeAvailable(fmt)
endif()

add_library(jlink INTERFACE)
target_include_directories(jlink INTERFACE src)
target_link_libraries(jlink INTERFACE ${libjlinkarm} fmt::fmt)

add_library(jlink::jlink ALIAS jlink)
