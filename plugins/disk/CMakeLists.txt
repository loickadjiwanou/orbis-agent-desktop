cmake_minimum_required(VERSION 3.20)
project(plugin_disk LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(nlohmann_json CONFIG REQUIRED)

add_library(disk MODULE disk.cpp)
target_link_libraries(disk PRIVATE nlohmann_json::nlohmann_json)

if(WIN32)
    target_compile_definitions(disk PRIVATE PLATFORM_WINDOWS)
elseif(APPLE)
    target_compile_definitions(disk PRIVATE PLATFORM_MACOS)
else()
    target_compile_definitions(disk PRIVATE PLATFORM_LINUX)
endif()

set_target_properties(disk PROPERTIES PREFIX "" OUTPUT_NAME "disk")
