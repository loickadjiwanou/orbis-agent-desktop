cmake_minimum_required(VERSION 3.20)
project(plugin_docker_stats LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(CURL REQUIRED)

add_library(docker_stats MODULE docker_stats.cpp)
target_link_libraries(docker_stats PRIVATE
    nlohmann_json::nlohmann_json
    CURL::libcurl
)

if(WIN32)
    target_compile_definitions(docker_stats PRIVATE PLATFORM_WINDOWS)
elseif(APPLE)
    target_compile_definitions(docker_stats PRIVATE PLATFORM_MACOS)
else()
    target_compile_definitions(docker_stats PRIVATE PLATFORM_LINUX)
endif()

set_target_properties(docker_stats PROPERTIES PREFIX "" OUTPUT_NAME "docker_stats")
