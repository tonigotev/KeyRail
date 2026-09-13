cmake_minimum_required(VERSION 3.15)

# CMP0091 lets CMAKE_MSVC_RUNTIME_LIBRARY select the CRT. It must be set before
# project() so the compiler check uses the same runtime as the real build.
cmake_policy(SET CMP0091 NEW)

project(keyraild CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Link the CRT statically. Dynamic linking makes the daemon depend on
# MSVCP140.dll and VCRUNTIME140.dll from the Visual C++ redistributable, which
# is missing on a clean Windows install and fails with an unhelpful error. The
# daemon is small enough that the size cost beats shipping a prerequisite.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(keyraild
    src/main.cpp
    src/action_factory.cpp
    src/app_audio.cpp
    src/audio.cpp
    src/browser_bridge.cpp
    src/clipboard_history.cpp
    src/config.cpp
    src/control_pipe.cpp
    src/discord_bridge.cpp
    src/display_state.cpp
    src/hotkey.cpp
    src/hotkey_registry.cpp
    src/media_sessions.cpp
    src/rescue.cpp
    src/rescue_kill.cpp
    src/rescue_meta.cpp
    src/rescue_nt.cpp
    src/rescue_overlay.cpp
    src/rescue_residency.cpp
    src/rescue_sampler.cpp
    src/rescue_trigger.cpp
    src/synthetic_hotkey_suppression.cpp
)

target_compile_definitions(keyraild PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(keyraild PRIVATE external)
target_link_libraries(keyraild PRIVATE advapi32 bcrypt gdi32 gdiplus ole32 runtimeobject shell32 user32 uxtheme version windowsapp windowscodecs ws2_32)

add_executable(media_list
    src/browser_bridge.cpp
    src/display_state.cpp
    src/media_list_main.cpp
    src/media_sessions.cpp
    src/synthetic_hotkey_suppression.cpp
)

target_compile_definitions(media_list PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(media_list PRIVATE external)
target_link_libraries(media_list PRIVATE bcrypt gdi32 gdiplus runtimeobject shell32 user32 windowsapp windowscodecs ws2_32)

# Rescue menu development tools. Neither ships: rescue_probe dumps the ranked
# process table the overlay would show, rescue_stress produces the failure
# modes it is meant to survive. build.ps1 does not copy them into dist\.
add_executable(rescue_probe
    src/rescue_nt.cpp
    src/rescue_probe_main.cpp
    src/rescue_sampler.cpp
)

target_compile_definitions(rescue_probe PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(rescue_probe PRIVATE external)
target_link_libraries(rescue_probe PRIVATE user32)

add_executable(rescue_stress
    stress/stress_main.cpp
)

target_compile_definitions(rescue_stress PRIVATE NOMINMAX UNICODE _UNICODE)
target_link_libraries(rescue_stress PRIVATE d3d11 dxgi gdi32 user32)

if (MSVC)
    target_compile_options(keyraild PRIVATE /W4 /utf-8)
    target_compile_options(media_list PRIVATE /W4 /utf-8)
    target_compile_options(rescue_probe PRIVATE /W4 /utf-8)
    target_compile_options(rescue_stress PRIVATE /W4 /utf-8)
    target_link_options(keyraild PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

# The daemon runs as a background Windows process. media_list, rescue_probe and
# rescue_stress stay console tools.
