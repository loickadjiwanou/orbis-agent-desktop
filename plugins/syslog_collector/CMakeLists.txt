cmake_minimum_required(VERSION 3.20)
project(plugin_syslog_collector LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(nlohmann_json CONFIG REQUIRED)

add_library(syslog_collector MODULE syslog_collector.cpp)
target_link_libraries(syslog_collector PRIVATE nlohmann_json::nlohmann_json)

if(WIN32)
    target_compile_definitions(syslog_collector PRIVATE PLATFORM_WINDOWS)
    target_link_libraries(syslog_collector PRIVATE advapi32)
elseif(APPLE)
    target_compile_definitions(syslog_collector PRIVATE PLATFORM_MACOS)
else()
    target_compile_definitions(syslog_collector PRIVATE PLATFORM_LINUX)
endif()

set_target_properties(syslog_collector PROPERTIES PREFIX "" OUTPUT_NAME "syslog_collector")
