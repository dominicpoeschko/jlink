cmake_minimum_required(VERSION 3.28)

project(jlink)

find_library(libjlinkarm jlinkarm REQUIRED HINTS /opt/SEGGER/JLink)

add_library(jlink INTERFACE)
target_include_directories(jlink INTERFACE src)
target_link_libraries(jlink INTERFACE ${libjlinkarm})

add_library(jlink::jlink ALIAS jlink)
