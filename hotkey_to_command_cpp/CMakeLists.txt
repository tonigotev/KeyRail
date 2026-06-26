cmake_minimum_required(VERSION 3.15)
project(hotkeyd CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(hotkeyd
    src/main.cpp
    src/action_factory.cpp
    src/audio.cpp
    src/config.cpp
    src/control_pipe.cpp
    src/hotkey.cpp
    src/hotkey_registry.cpp
)

target_compile_definitions(hotkeyd PRIVATE UNICODE _UNICODE)
target_include_directories(hotkeyd PRIVATE external)
target_link_libraries(hotkeyd PRIVATE ole32 user32)

if (MSVC)
    target_compile_options(hotkeyd PRIVATE /W4 /utf-8)
endif()

# Console subsystem for now (so you see the prints). When we add the tray icon
# later, switch to WIN32 to drop the console:
#   set_target_properties(hotkeyd PROPERTIES WIN32_EXECUTABLE TRUE)
