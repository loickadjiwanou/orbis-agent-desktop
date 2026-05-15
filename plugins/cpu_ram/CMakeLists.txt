cmake_minimum_required(VERSION 3.20)
project(plugin_cpu_ram LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(nlohmann_json CONFIG REQUIRED)

add_library(cpu_ram MODULE cpu_ram.cpp)
target_link_libraries(cpu_ram PRIVATE nlohmann_json::nlohmann_json)

if(WIN32)
    target_compile_definitions(cpu_ram PRIVATE PLATFORM_WINDOWS)
elseif(APPLE)
    target_compile_definitions(cpu_ram PRIVATE PLATFORM_MACOS)
else()
    target_compile_definitions(cpu_ram PRIVATE PLATFORM_LINUX)
endif()

set_target_properties(cpu_ram PROPERTIES
    PREFIX ""
    OUTPUT_NAME "cpu_ram"
)
