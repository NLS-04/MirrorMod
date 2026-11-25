cmake_minimum_required(VERSION 3.15)

# The single source of the mod's version. vcpkg.json and CHANGELOG.md must agree with it, which the CI checks.
project(MirrorMod VERSION 1.0.0 LANGUAGES CXX RC)

# Appended to the version shown in the mod and in the DLL's properties, such as "-dev.12+3f2a1bc" for CI builds.
# Empty for releases.
set(MIRRORMOD_VERSION_SUFFIX "" CACHE STRING "Suffix appended to the version, empty for releases")
set(MIRRORMOD_VERSION_FULL "${PROJECT_VERSION}${MIRRORMOD_VERSION_SUFFIX}")

# Must match the SDK installed in the game folder (see Retail/ZHMModSDK.dll).
# Set the ZHMMODSDK_DIR variable to use a local copy of the ZHMModSDK instead.
set(ZHMMODSDK_VER "v4.2.1")
include(cmake/setup-zhmmodsdk.cmake)

set(CMAKE_CXX_STANDARD 23)

option(MIRRORMOD_BUILD_TESTS "Build the standalone tests, which need neither the SDK nor the game" OFF)

find_package(directx-headers CONFIG REQUIRED)
find_package(minhook CONFIG REQUIRED)

add_compile_options(/utf-8)

# The SDK's headers use std::min, which the macros from windows.h would break depending on the include order.
add_compile_definitions(NOMINMAX)

# Everything that does not depend on the SDK, so that the tests can use it without the game.
add_library(MirrorModCore STATIC
    src/audio/AudioHooks.cpp
    src/audio/AudioHooks.h
    src/core/GameHooks.cpp
    src/core/GameHooks.h
    src/core/GameMemory.cpp
    src/core/GameMemory.h
    src/core/Hotkey.cpp
    src/core/Hotkey.h
    src/core/Log.cpp
    src/core/Log.h
    src/core/VtableHook.cpp
    src/core/VtableHook.h
    src/diagnostics/UiTrace.cpp
    src/diagnostics/UiTrace.h
    src/game/SceneEvents.cpp
    src/game/SceneEvents.h
    src/game/ShoulderSwap.cpp
    src/game/ShoulderSwap.h
    src/hud/HudMirror.cpp
    src/hud/HudMirror.h
    src/hud/MapMirror.cpp
    src/hud/MapMirror.h
    src/hud/Scaleform.h
    src/render/CommandListHooks.cpp
    src/render/CommandListHooks.h
    src/render/MirrorFlip.cpp
    src/render/MirrorFlip.h
)

target_include_directories(MirrorModCore PUBLIC src)

target_link_libraries(MirrorModCore PUBLIC
    Microsoft::DirectX-Guids
    Microsoft::DirectX-Headers
    minhook::minhook
)

add_library(MirrorMod SHARED
    src/MirrorMod.cpp
    src/MirrorMod.h
    src/MirrorModUi.cpp
    src/Settings.cpp
    src/Settings.h
    src/diagnostics/UiObjectDescriber.cpp
    src/diagnostics/UiObjectDescriber.h
    src/game/SceneState.cpp
    src/game/SceneState.h
    src/render/FrameMirror.cpp
    src/render/FrameMirror.h
)

target_compile_definitions(MirrorMod PRIVATE MIRRORMOD_VERSION="${MIRRORMOD_VERSION_FULL}")

# The version shown in the DLL's file properties.
configure_file(cmake/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)
target_sources(MirrorMod PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/version.rc")

target_link_libraries(MirrorMod
    MirrorModCore
    ZHMModSDK
    # Not used by the mod, but the SDK's IPluginInterface sets the ImPlot context.
    implot
    d3d12
    d3dcompiler
    version
)

install(TARGETS MirrorMod
    RUNTIME DESTINATION bin
)

# Copies the mod to the game folder only when GAME_INSTALL_PATH is set.
zhmmodsdk_install(MirrorMod)

if (MIRRORMOD_BUILD_TESTS)
    enable_testing()

    function(add_mirror_test p_Name)
        add_executable(${p_Name} tests/${p_Name}.cpp)
        target_link_libraries(${p_Name} PRIVATE MirrorModCore ${ARGN})
        add_test(NAME ${p_Name} COMMAND ${p_Name})
    endfunction()

    add_mirror_test(CoreTest)
    add_mirror_test(HudMirrorTest)
    add_mirror_test(UiTraceTest)
    add_mirror_test(AudioSwapTest)

    # Exercises the flip pass and the command list hooks on the real GPU.
    add_mirror_test(MirrorFlipTest d3d12 dxgi d3dcompiler)
endif()
