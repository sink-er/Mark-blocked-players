# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.22)

# Allow this repository to be configured either standalone or as a subdirectory
# of a larger RE-UE4SS workspace.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(MBPId LANGUAGES CXX)
endif()

set(TARGET MBPId)

if(NOT TARGET UE4SS)
    set(UE4SS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../RE-UE4SS"
        CACHE PATH "Path to a RE-UE4SS source checkout")
    if(NOT EXISTS "${UE4SS_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "The UE4SS target is not available. Clone RE-UE4SS next to this "
            "repository or configure with -DUE4SS_SOURCE_DIR=<path-to-RE-UE4SS>.")
    endif()
    add_subdirectory("${UE4SS_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/RE-UE4SS")
endif()

# 按功能模块拆分：dllmain 只做装配 + 游戏线程调度，各模块自带 .hpp/.cpp。
add_library(${TARGET} SHARED
    src/dllmain.cpp
    src/common.cpp
    src/blocklist.cpp
    src/ui_strings.cpp
    src/players.cpp
    src/names.cpp
    src/server_list.cpp
    src/card_panel.cpp
    src/profile_legacy.cpp
)

target_compile_features(${TARGET} PRIVATE cxx_std_23)
target_link_libraries(${TARGET} PUBLIC UE4SS)

# Optional deployment. Leave empty for a clean source tree; pass
# -DMBP_DEPLOY_DIR=<...>/ue4ss/Mods/MBPId/dlls to copy main.dll after build.
set(MBP_DEPLOY_DIR "" CACHE PATH "Optional UE4SS MBPId/dlls deployment directory")
if(MBP_DEPLOY_DIR)
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MBP_DEPLOY_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${TARGET}>" "${MBP_DEPLOY_DIR}/main.dll"
        COMMENT "Deploying ${TARGET}.dll to ${MBP_DEPLOY_DIR}/main.dll"
    )
endif()
