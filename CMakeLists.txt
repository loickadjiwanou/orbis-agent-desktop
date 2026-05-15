cmake_minimum_required(VERSION 3.20)
project(orbis-agent VERSION 1.0.0 LANGUAGES CXX)

# RC language (Windows resource compiler) is only available on Windows
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# MSVC runtime library — must match the vcpkg triplet's CRT linkage.
# x64-windows-static builds all deps with /MT; mismatching causes LNK2038.
# ---------------------------------------------------------------------------
if(MSVC AND DEFINED VCPKG_TARGET_TRIPLET AND VCPKG_TARGET_TRIPLET MATCHES "-static")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---------------------------------------------------------------------------
# vcpkg toolchain (auto-detect VCPKG_ROOT env var)
# ---------------------------------------------------------------------------
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain")
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(eclipse-paho-mqtt-c CONFIG REQUIRED)
find_package(nlohmann_json       CONFIG REQUIRED)
find_package(spdlog              CONFIG REQUIRED)
find_package(SQLite3                    REQUIRED)
find_package(OpenSSL                    REQUIRED)
find_package(CURL                       REQUIRED)
find_package(toml11              CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Main executable sources
# ---------------------------------------------------------------------------
add_executable(orbis-agent
    src/main.cpp
    src/config/ConfigManager.cpp
    src/mqtt/MQTTClient.cpp
    src/core/MessageQueue.cpp
    src/services/LogCollector.cpp
    src/services/CommandExecutor.cpp
    src/services/HeartbeatService.cpp
    src/services/UpdateManager.cpp
    src/services/NetworkScanner.cpp
    src/buffer/LocalBuffer.cpp
    src/plugins/PluginLoader.cpp
)

# Platform-specific service implementation
if(WIN32)
    target_sources(orbis-agent PRIVATE
        src/platform/WindowsService.cpp
        # Resource file: embeds UAC manifest + version info
        packaging/windows/orbis-agent.rc
    )
    target_compile_definitions(orbis-agent PRIVATE PLATFORM_WINDOWS _WIN32_WINNT=0x0A00)
    target_link_libraries(orbis-agent PRIVATE ws2_32 advapi32 psapi)
    # Embed the custom manifest via linker — disables auto-generated manifest
    if(MSVC)
        # The manifest is already embedded via the .rc file (RT_MANIFEST resource).
        # Disable the linker's auto-manifest to avoid CVT1100 duplicate resource error.
        target_link_options(orbis-agent PRIVATE /MANIFEST:NO)
    endif()
elseif(APPLE)
    target_sources(orbis-agent PRIVATE src/platform/MacOSService.cpp)
    target_compile_definitions(orbis-agent PRIVATE PLATFORM_MACOS)
    target_link_libraries(orbis-agent PRIVATE
        "-framework IOKit"
        "-framework CoreFoundation"
    )
else()
    target_sources(orbis-agent PRIVATE src/platform/LinuxService.cpp)
    target_compile_definitions(orbis-agent PRIVATE PLATFORM_LINUX)
    target_link_libraries(orbis-agent PRIVATE pthread dl)
endif()

# ---------------------------------------------------------------------------
# Include paths
# ---------------------------------------------------------------------------
target_include_directories(orbis-agent PRIVATE src)

# ---------------------------------------------------------------------------
# Link libraries
# ---------------------------------------------------------------------------
target_link_libraries(orbis-agent PRIVATE
    eclipse-paho-mqtt-c::paho-mqtt3cs-static
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    SQLite3::SQLite3
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    toml11::toml11
)

# ---------------------------------------------------------------------------
# Compiler warnings
# ---------------------------------------------------------------------------
if(MSVC)
    target_compile_options(orbis-agent PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(orbis-agent PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        -Wno-unused-parameter   # spdlog macros sometimes trigger this
    )
endif()

# ---------------------------------------------------------------------------
# Plugins (shared libraries loaded at runtime via dlopen/LoadLibrary)
# ---------------------------------------------------------------------------
add_subdirectory(plugins/cpu_ram)
add_subdirectory(plugins/disk)
add_subdirectory(plugins/docker_stats)
add_subdirectory(plugins/syslog_collector)

# Output all plugin .so/.dll files directly into build/plugins/ (flat layout).
# This matches the plugins_dir expected by PluginLoader (no subdirectory scan).
set_target_properties(cpu_ram disk docker_stats syslog_collector PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"  # Windows DLL path
)

# ---------------------------------------------------------------------------
# Install rules
# ---------------------------------------------------------------------------
if(UNIX AND NOT APPLE)
    # ── Linux ──────────────────────────────────────────────────────────────
    install(TARGETS orbis-agent DESTINATION usr/bin)
    install(TARGETS cpu_ram disk docker_stats syslog_collector
            LIBRARY DESTINATION usr/lib/orbis-agent/plugins)
    install(FILES   packaging/linux/orbis-agent.service
            DESTINATION lib/systemd/system)
    install(FILES   packaging/linux/config.toml.default
            DESTINATION etc/orbis-agent
            RENAME config.toml)
elseif(APPLE)
    # ── macOS ──────────────────────────────────────────────────────────────
    install(TARGETS orbis-agent DESTINATION usr/local/bin)
    install(TARGETS cpu_ram disk docker_stats syslog_collector
            LIBRARY DESTINATION usr/local/lib/orbis-agent/plugins)
    install(FILES   packaging/macos/com.orbis.agent.plist
            DESTINATION Library/LaunchDaemons)
    install(FILES   packaging/macos/config.toml.default
            DESTINATION etc/orbis-agent
            RENAME config.toml)
else()
    # ── Windows ────────────────────────────────────────────────────────────
    install(TARGETS orbis-agent DESTINATION ".")
    install(TARGETS cpu_ram disk docker_stats syslog_collector
            LIBRARY DESTINATION plugins
            RUNTIME DESTINATION plugins)
    install(FILES   packaging/windows/config.toml.default
            DESTINATION "."
            RENAME config.toml)
endif()

# ---------------------------------------------------------------------------
# CPack — produce .deb / .dmg / .exe from `cpack` after cmake --build
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME                "orbis-agent")
set(CPACK_PACKAGE_VENDOR              "Orbis")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Orbis remote monitoring and management agent")
set(CPACK_PACKAGE_VERSION_MAJOR       ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR       ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH       ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_VERSION             ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT             "support@orbis.io")

if(UNIX AND NOT APPLE)
    # ── DEB ────────────────────────────────────────────────────────────────
    set(CPACK_GENERATOR "DEB")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Orbis <support@orbis.io>")
    set(CPACK_DEBIAN_PACKAGE_SECTION    "admin")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY   "optional")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS    "libssl3 | libssl1.1, libcurl4, libsqlite3-0")
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/postinst"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/prerm"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/postrm")
    set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")

elseif(APPLE)
    # ── DragNDrop (DMG) ────────────────────────────────────────────────────
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "Orbis Agent")
    if(DEFINED ORBIS_ARCH_LABEL)
        set(CPACK_PACKAGE_FILE_NAME "orbis-agent-${PROJECT_VERSION}-macos-${ORBIS_ARCH_LABEL}")
    else()
        set(CPACK_PACKAGE_FILE_NAME "orbis-agent-${PROJECT_VERSION}-macos")
    endif()

else()
    # ── NSIS (EXE) ─────────────────────────────────────────────────────────
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_NSIS_DISPLAY_NAME              "Orbis Agent")
    set(CPACK_NSIS_PACKAGE_NAME              "Orbis Agent")
    set(CPACK_NSIS_INSTALL_ROOT              "C:\\\\Program Files")
    set(CPACK_NSIS_EXECUTABLES_DIRECTORY     ".")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)

    # Request UAC elevation for the installer itself
    set(CPACK_NSIS_DEFINES "RequestExecutionLevel admin")

    # Copy NSIS extra-command scripts to the build directory at configure time.
    # Using external .nsi files avoids CMake string-escaping issues when CPack
    # serialises multi-line NSIS code into CPackConfig.cmake.
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/nsis_install_commands.nsi"
        "${CMAKE_CURRENT_BINARY_DIR}/nsis_install_commands.nsi"
        COPYONLY)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/nsis_uninstall_commands.nsi"
        "${CMAKE_CURRENT_BINARY_DIR}/nsis_uninstall_commands.nsi"
        COPYONLY)

    # CMake normalises paths with forward slashes — NSIS !include accepts them.
    # No quotes around the path: CPack does not escape inner " when serialising
    # to CPackConfig.cmake, which breaks the !include into zero arguments.
    # GitHub Actions runner paths never contain spaces so quoting is not needed.
    set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS
        "!include ${CMAKE_CURRENT_BINARY_DIR}/nsis_install_commands.nsi")
    set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS
        "!include ${CMAKE_CURRENT_BINARY_DIR}/nsis_uninstall_commands.nsi")

    set(CPACK_PACKAGE_FILE_NAME "orbis-agent-${PROJECT_VERSION}-windows-x64-setup")
endif()

include(CPack)
