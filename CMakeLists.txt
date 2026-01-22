cmake_minimum_required(VERSION 3.28)

project(jlink)

set(JLINK_OPTIONAL
    ON
    CACHE BOOL "Make jlinkarm library optional")

if(JLINK_OPTIONAL)
    set(jlink_requirement OPTIONAL)
else()
    set(jlink_requirement REQUIRED)
endif()

find_library(libjlinkarm jlinkarm ${jlink_requirement} HINTS /opt/SEGGER/JLink)

add_library(jlink INTERFACE)
target_include_directories(jlink INTERFACE src)
if(libjlinkarm)
    target_link_libraries(jlink INTERFACE ${libjlinkarm})
endif()

add_library(jlink::jlink ALIAS jlink)
