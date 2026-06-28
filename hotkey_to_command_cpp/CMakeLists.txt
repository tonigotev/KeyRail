cmake_minimum_required(VERSION 3.15)
project(hotkeyd CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(hotkeyd
    src/main.cpp
    src/action_factory.cpp
    src/app_audio.cpp
    src/audio.cpp
    src/browser_bridge.cpp
    src/config.cpp
    src/control_pipe.cpp
    src/discord_bridge.cpp
    src/hotkey.cpp
    src/hotkey_registry.cpp
    src/media_sessions.cpp
    src/synthetic_hotkey_suppression.cpp
)

target_compile_definitions(hotkeyd PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(hotkeyd PRIVATE external)
target_link_libraries(hotkeyd PRIVATE bcrypt gdi32 ole32 runtimeobject user32 windowsapp ws2_32)

add_executable(media_list
    src/browser_bridge.cpp
    src/media_list_main.cpp
    src/media_sessions.cpp
    src/synthetic_hotkey_suppression.cpp
)

target_compile_definitions(media_list PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(media_list PRIVATE external)
target_link_libraries(media_list PRIVATE bcrypt gdi32 runtimeobject user32 windowsapp ws2_32)

if (MSVC)
    target_compile_options(hotkeyd PRIVATE /W4 /utf-8)
    target_compile_options(media_list PRIVATE /W4 /utf-8)
endif()

# Console subsystem for now (so you see the prints). When we add the tray icon
# later, switch to WIN32 to drop the console:
#   set_target_properties(hotkeyd PROPERTIES WIN32_EXECUTABLE TRUE)
