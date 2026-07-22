cmake_minimum_required(VERSION 3.28)

project(jlink)

set(USE_JLINK_OPTIONAL
    OFF
    CACHE BOOL "Make jlinkarm library optional")

set(JLINK_USE_FAKE_DLL
    OFF
    CACHE BOOL "Use an in-process fake of the jlink DLL for testing without hardware")

add_library(jlink INTERFACE)
target_include_directories(jlink INTERFACE src)

if(JLINK_USE_FAKE_DLL)
    add_library(jlink_fake_dll STATIC src/jlink/JLinkFakeDLL.cpp)
    target_compile_features(jlink_fake_dll PUBLIC cxx_std_20)
    target_link_libraries(jlink INTERFACE jlink_fake_dll)
else()
    if(USE_JLINK_OPTIONAL)
        set(jlink_requirement OPTIONAL)
    else()
        set(jlink_requirement REQUIRED)
    endif()

    find_library(libjlinkarm jlinkarm ${jlink_requirement} HINTS /opt/SEGGER/JLink)

    if(libjlinkarm)
        target_link_libraries(jlink INTERFACE ${libjlinkarm})
    endif()
endif()

add_library(jlink::jlink ALIAS jlink)
