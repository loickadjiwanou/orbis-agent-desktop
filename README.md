# Orbis-agent — Desktop Agent C++17

Cross-platform system monitoring agent for the Orbis project. It installs as an OS service (systemd / launchd / Windows SCM), connects to the MQTT broker over TLS, and continuously reports metrics, logs, and command results to the Orbis backend.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Architecture and File Structure](#2-architecture-and-file-structure)
3. [Internal Components](#3-internal-components)
4. [Plugins](#4-plugins)
5. [Configuration](#5-configuration)
6. [MQTT Protocol — Session 0 Contract](#6-mqtt-protocol--session-0-contract)
7. [Build on macOS](#7-build-on-macos)
8. [Build on Linux](#8-build-on-linux)
9. [Build on Windows](#9-build-on-windows)
10. [Dependencies (vcpkg)](#10-dependencies-vcpkg)
11. [Installing as an OS Service](#11-installing-as-an-os-service)
12. [CLI Reference](#12-cli-reference)
13. [Troubleshooting](#13-troubleshooting)

---

## 1. Overview

`orbis-agent` is a C++17 executable that:

- Connects to the Orbis MQTT broker over TLS (default port 8883)
- Automatically registers itself with the backend on first start (onboarding flow)
- Publishes a **heartbeat** every 30 seconds (CPU, RAM, disk, network metrics)
- Collects **system logs** (journalctl / log show / Windows Event Log) and publishes them in batches
- Executes **remote commands** sent from the dashboard (shell, restart, network scan, etc.)
- Loads **native plugins** (.so / .dylib / .dll) to extend data collection
- Stores messages in a **local SQLite buffer** when the broker is unreachable, then replays them on reconnection
- Automatically monitors and restarts internal services (watchdog every 5 seconds)
- Installs and manages itself as a real OS service via the CLI

---

## 2. Architecture and File Structure

```
orbis-agent-desktop/
│
├── CMakeLists.txt              # Root build system (cmake >= 3.20)
├── vcpkg.json                  # vcpkg dependency manifest
├── config.toml.example         # Annotated configuration template
├── README.md                   # This file
│
├── src/
│   ├── constants.hpp           # All constants (topics, timeouts, paths)
│   ├── main.cpp                # Entry point: CLI parsing, onboarding, watchdog
│   │
│   ├── config/
│   │   ├── ConfigManager.hpp   # Meyers singleton — TOML config + token encryption
│   │   └── ConfigManager.cpp
│   │
│   ├── mqtt/
│   │   ├── MQTTClient.hpp      # Eclipse Paho C wrapper — TLS, exponential backoff
│   │   └── MQTTClient.cpp
│   │
│   ├── core/
│   │   ├── MessageQueue.hpp    # Bounded thread-safe FIFO (condition_variable)
│   │   └── MessageQueue.cpp
│   │
│   ├── buffer/
│   │   ├── LocalBuffer.hpp     # SQLite WAL persistence — offline message storage
│   │   └── LocalBuffer.cpp
│   │
│   ├── plugins/
│   │   ├── PluginLoader.hpp    # Runtime loading via dlopen / LoadLibrary
│   │   └── PluginLoader.cpp
│   │
│   ├── services/
│   │   ├── HeartbeatService.hpp/.cpp    # System metrics → MQTT (retained)
│   │   ├── LogCollector.hpp/.cpp        # OS logs + plugins → MQTT (batches of 100)
│   │   ├── CommandExecutor.hpp/.cpp     # Remote command execution
│   │   ├── UpdateManager.hpp/.cpp       # Agent self-update lifecycle
│   │   └── NetworkScanner.hpp/.cpp      # TCP network scan (CIDR, configurable ports)
│   │
│   └── platform/
│       ├── ServiceInstaller.hpp         # Abstract interface (factory method)
│       ├── LinuxService.cpp             # systemd unit generation + enable/start
│       ├── MacOSService.cpp             # launchd plist generation + launchctl load
│       └── WindowsService.cpp           # SCM CreateService + recovery actions
│
└── plugins/
    ├── cpu_ram/
    │   ├── CMakeLists.txt
    │   └── cpu_ram.cpp         # CPU % + RAM MB — Linux / macOS / Windows
    ├── disk/
    │   ├── CMakeLists.txt
    │   └── disk.cpp            # Disk usage per mount point
    ├── docker_stats/
    │   ├── CMakeLists.txt
    │   └── docker_stats.cpp    # Docker container stats via Unix socket
    └── syslog_collector/
        ├── CMakeLists.txt
        └── syslog_collector.cpp  # journalctl / log show / ReadEventLog
```

---

## 3. Internal Components

### ConfigManager (`src/config/`)

Meyers singleton. Reads a TOML file using `toml11`. Key features:

- **Auto-generated UUID v4** if `device_id` is empty on first start
- **AES-256-CBC encrypted token** stored on disk — the key is derived from the machine hostname via SHA-256 (OpenSSL EVP). The token is never stored in plaintext.
- **Regex-based persistence**: rewrites `device_id = "..."` and `token = "..."` directly in the TOML file without reformatting it.
- Exposes typed sub-structs: `MQTTConfig`, `LogConfig`, `HeartbeatConfig`, `PluginConfig`, `NetworkScannerConfig`.

### MQTTClientWrapper (`src/mqtt/`)

Wrapper around the **Eclipse Paho MQTT C** library (synchronous API, `MQTTClient.h`).

- TLS connection with optional CA certificate verification (`MQTTClient_SSLOptions`)
- **Last Will** support (`retained=true`) to signal unexpected disconnections
- **Exponential backoff reconnection** in a detached thread: `delay = min(max, min × 2^attempt)`
- Automatic re-subscription to all topics after reconnection
- Paho callbacks bridged via static functions + `void* context` pointer

### LocalBuffer (`src/buffer/`)

SQLite3 (C API) in WAL mode with synchronous=NORMAL.

- Stores unpublished messages: `(topic TEXT, payload TEXT, timestamp INTEGER)`
- Flushes in batches of 50 when the connection is restored
- Automatically prunes the oldest rows when the table exceeds 100,000 entries
- RAII: `StmtGuard` automatically finalizes prepared statements

### MessageQueue (`src/core/`)

Bounded thread-safe FIFO. `push()` is non-blocking (drops if full), `pop()` blocks using `condition_variable::wait_for` with a configurable timeout.

### HeartbeatService (`src/services/`)

Named thread (`pthread_setname_np` on Linux). Publishes every N seconds to `devices/{id}/status` with `retained=true`. Metrics are collected natively per platform:

| Platform | CPU | RAM | Disk |
|---|---|---|---|
| Linux | `/proc/stat` delta | `/proc/meminfo` | `statvfs` |
| macOS | `host_processor_info` | `vm_statistics64` + `sysctlbyname` | `statvfs` |
| Windows | `GetSystemTimes` delta | `GlobalMemoryStatusEx` | `GetDiskFreeSpaceEx` |

On `stop()`, publishes a final `"status": "offline"` message.

### LogCollector (`src/services/`)

Collects logs from loaded plugins and OS sources. Groups them into batches of 100 before publishing (per Section 14 of the Session 0 contract). Falls back to `LocalBuffer` when MQTT is offline. `triggerNow()` wakes the sleep loop atomically.

### CommandExecutor (`src/services/`)

Subscribes to `devices/{id}/commands` and `devices/{id}/update`. Uses a **dedicated worker thread** to decouple the Paho MQTT callback from command execution:

- `onMessageArrived` (Paho callback) only enqueues the raw payload — never blocks
- The worker thread drains the queue and runs each command in a publish-safe context

Command lifecycle per incoming message:

1. Publishes `"statut": "acknowledged"`
2. Publishes `"statut": "executing"`
3. Publishes `"statut": "success"` or `"failed"` with `output`, `error`, `exit_code`

> **Why the worker thread matters**: calling `publish()` directly from the Paho message callback (or calling `MQTTClient_waitForCompletion` from any application thread) can deadlock the Paho internal receive thread, causing MQTT disconnects within 1-2 seconds of the first command.

Result format (Session 0, Section 6):
```json
{
  "command_id": "...",
  "device_id": "...",
  "statut": "success",
  "output": "...",
  "error": null,
  "exit_code": 0,
  "timestamp": "2025-05-05T10:00:00Z"
}
```

Shell timeout is implemented via `std::async` + `future::wait_for`. Output is capped at 64 KB.

### UpdateManager (`src/services/`)

Full self-update lifecycle:
1. Download the new binary via libcurl
2. Verify the SHA-256 checksum (OpenSSL EVP)
3. Back up the current executable
4. Atomic replacement (`rename()` on POSIX / `MoveFileEx` on Windows)
5. Automatic rollback on any failure

### NetworkScanner (`src/services/`)

Detects the machine's subnet (`getifaddrs` / `GetAdaptersAddresses`), expands it as a CIDR range (min /16, max 254 hosts). Launches 32 concurrent `std::async` tasks. Uses non-blocking TCP `connect` with `select()` and a configurable timeout per port probe.

---

## 4. Plugins

Plugins are **shared libraries** (`.so` / `.dylib` / `.dll`) loaded at runtime via `dlopen` (POSIX) or `LoadLibrary` (Windows).

### Required ABI

Each plugin must export exactly these 4 C symbols:

```cpp
extern "C" {
    const char* orbis_plugin_name();     // Unique plugin identifier
    const char* orbis_plugin_version();  // SemVer version string
    const char* collect();               // Returns a JSON array as a C string
    void        cleanup();               // Releases resources
}
```

The return format of `collect()` is a JSON array of objects:

```json
[
  {
    "source": "cpu_ram_plugin",
    "level": "INFO",
    "message": "CPU 12.3% RAM 4096 MB",
    "metadata": {
      "cpu_percent": 12.3,
      "ram_used_mb": 4096.0
    }
  }
]
```

### Bundled Plugins

| Plugin | Description | Platform |
|---|---|---|
| `cpu_ram` | CPU % and RAM MB (used / total) | Linux, macOS, Windows |
| `disk` | Usage per mount point, alert if >75% or >90% | Linux, macOS, Windows |
| `docker_stats` | CPU/RAM for Docker containers via Unix socket | Linux, macOS |
| `syslog_collector` | System logs (journalctl / log show / EventLog) | Linux, macOS, Windows |

### Adding a Custom Plugin

1. Create a folder `plugins/my_plugin/`
2. Implement the 4 required C symbols above
3. Add a minimal `CMakeLists.txt`:
   ```cmake
   cmake_minimum_required(VERSION 3.20)
   project(plugin_my_plugin LANGUAGES CXX)
   set(CMAKE_CXX_STANDARD 17)
   find_package(nlohmann_json CONFIG REQUIRED)
   add_library(my_plugin MODULE my_plugin.cpp)
   target_link_libraries(my_plugin PRIVATE nlohmann_json::nlohmann_json)
   set_target_properties(my_plugin PROPERTIES PREFIX "" OUTPUT_NAME "my_plugin")
   ```
4. Add `add_subdirectory(plugins/my_plugin)` to the root `CMakeLists.txt`
5. Drop the compiled `.so` / `.dylib` / `.dll` into the `plugins_dir` configured in `config.toml`

---

## 5. Configuration

Copy `config.toml.example` to the expected path for your platform:

```
Linux / macOS : /etc/orbis-agent/config.toml
Windows       : C:\ProgramData\Orbis\config.toml
```

### Sections

```toml
[agent]
device_id = ""          # Auto-generated on first start — DO NOT EDIT MANUALLY
token     = ""          # Received from the backend after onboarding — DO NOT EDIT MANUALLY
log_file  = "/var/log/orbis-agent.log"

[mqtt]
broker_host             = "my-server.example.com"
broker_port             = 8883
use_tls                 = true
ca_cert_path            = ""      # Path to CA.crt — empty = skip certificate verification
register_secret         = "orbis_register_secret"
reconnect_delay_min_sec = 1
reconnect_delay_max_sec = 60
keepalive_sec           = 60

[logs]
interval_sec = 60
levels       = ["INFO", "WARNING", "ERROR", "CRITICAL"]

[heartbeat]
interval_sec = 30

[plugins]
enabled     = true
plugins_dir = "/usr/lib/orbis-agent/plugins"

[network_scanner]
enabled           = false
scan_interval_sec = 300
port_range        = "22,80,443,8080,8883"
```

### Auto-managed Fields

`device_id` and `token` are written automatically by the agent during onboarding. Do not edit them manually. The `token` is stored **AES-256 encrypted** on disk.

---

## 6. MQTT Protocol — Session 0 Contract

The agent strictly implements the contract defined in `docs/SESSION_0_CONTRAT_INTERFACES.md`.

### Onboarding Flow (first start)

```
Agent                              Broker                   Backend
  |                                   |                        |
  |-- connect(user="register") ------>|                        |
  |-- subscribe(devices/{id}/register_ack) -->|               |
  |-- publish(devices/register, payload) --->|                |
  |                                   |------ forward -------->|
  |                                   |<----- register_ack ----|
  |<-- message(devices/{id}/register_ack) ---|                |
  |-- disconnect() ------------------>|                        |
  |-- reconnect(user=device_id, password=token) ->|           |
```

### Topic Reference

| Direction | Topic | Description |
|---|---|---|
| → Publish | `devices/register` | Initial registration payload |
| ← Subscribe | `devices/{id}/register_ack` | Token received from the backend |
| → Publish | `devices/{id}/status` | Heartbeat (retained=true) |
| → Publish | `devices/{id}/logs` | Log batch |
| ← Subscribe | `devices/{id}/commands` | Remote commands |
| → Publish | `devices/{id}/results` | Command execution results |
| ← Subscribe | `devices/{id}/update` | Agent update payload |
| → Publish | `devices/{id}/discovery` | Network scan results |

---

## 7. Build on macOS

### Prerequisites

| Tool | Minimum Version | Installation |
|---|---|---|
| Xcode or CLT | 14+ | `xcode-select --install` |
| CMake | 3.20+ | `brew install cmake` |
| Ninja | any | `brew install ninja` |
| vcpkg | latest | see below |

### Installing vcpkg

```bash
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh

# Add to ~/.zshrc or ~/.bashrc
export VCPKG_ROOT=~/vcpkg
```

### Building the Agent

```bash
cd orbis-agent-desktop

# Configure — installs all vcpkg dependencies on first run (~15 min)
cmake -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_CXX_COMPILER=$(xcrun -f clang++)

# Compile
cmake --build build

# Verify
./build/orbis-agent --version
```

### Release Build (optimized)

```bash
cmake -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_CXX_COMPILER=$(xcrun -f clang++)

cmake --build build-release
```

### Build Artifacts

```
build/
├── orbis-agent          # Main executable
└── plugins/
    ├── cpu_ram.so
    ├── disk.so
    ├── docker_stats.so
    └── syslog_collector.so
```

### macOS-Specific Notes

- If Xcode is installed at a non-standard path, always pass `$(xcrun -f clang++)` as the compiler
- `_NSGetExecutablePath` requires `<mach-o/dyld.h>` (included automatically in relevant source files)
- `sysctlbyname` requires `<sys/sysctl.h>` (included automatically)
- `getpagesize()` requires `<unistd.h>` (included in plugins)
- The `docker_stats` plugin uses `/var/run/docker.sock` — Docker Desktop must be running

---

## 8. Build on Linux

### Prerequisites

```bash
# Ubuntu / Debian
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build \
  git curl zip unzip tar \
  pkg-config

# Fedora / RHEL
sudo dnf install -y \
  gcc-c++ cmake ninja-build \
  git curl zip unzip tar \
  pkg-config
```

### Installing vcpkg

```bash
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh

# Add to ~/.bashrc
export VCPKG_ROOT=~/vcpkg
```

### Building the Agent

```bash
cd orbis-agent-desktop

# Configure — installs all vcpkg dependencies on first run (~15-30 min)
cmake -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

# Compile
cmake --build build

# Verify
./build/orbis-agent --version

# To run the agent on Linux
./build/orbis-agent --run --config config.toml

# To run the agent on macOS
./build/orbis-agent --run --config config.toml

# To run the agent on Windows
.\build\orbis-agent.exe --run --config config.toml

# To run the agent on Linux with a custom config file
./build/orbis-agent --run --config config.test.toml
```

### Release Build

```bash
cmake -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

cmake --build build-release
```

### Linux-Specific Notes

- `pthread` and `dl` are linked automatically via `CMakeLists.txt` (`-lpthread -ldl`)
- The `syslog_collector` plugin calls `journalctl` — systemd must be present on the system
- The `disk` plugin reads `/proc/mounts` — available on all Linux kernels
- `pthread_setname_np` is used to name internal threads (improves debugging with `top` / `htop`)
- On ARM (Raspberry Pi, AWS Graviton), the build process is identical — vcpkg handles the triplet automatically

---

## 9. Build on Windows

### Prerequisites

| Tool | Installation |
|---|---|
| Visual Studio 2022 | [visualstudio.microsoft.com](https://visualstudio.microsoft.com) — include the "Desktop development with C++" workload |
| CMake 3.20+ | Bundled with VS2022, or [cmake.org](https://cmake.org/download/) |
| Git | [git-scm.com](https://git-scm.com) |
| vcpkg | see below |

### Installing vcpkg

```powershell
# In PowerShell (as Administrator)
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat

# Set the environment variable permanently
[System.Environment]::SetEnvironmentVariable("VCPKG_ROOT", "C:\vcpkg", "User")
```

### Building from the Command Line (VS2022 Developer Command Prompt)

```bat
cd orbis-agent-desktop

REM Configure
cmake -B build -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake

REM Debug build
cmake --build build --config Debug

REM Release build
cmake --build build --config Release

REM Verify
build\Release\orbis-agent.exe --version
```

### Building with Ninja (faster)

```bat
REM Open "x64 Native Tools Command Prompt for VS 2022"
cd orbis-agent-desktop

cmake -B build -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake

cmake --build build
```

### Windows-Specific Notes

- `vcpkg.json` triggers automatic dependency installation on the first `cmake` run
- Required Windows libraries are linked automatically: `ws2_32`, `advapi32`, `psapi`
- `_WIN32_WINNT=0x0A00` is defined to target Windows 10 and above
- The `docker_stats` plugin is disabled on Windows (`#if !defined(PLATFORM_WINDOWS)`)
- The `syslog_collector` plugin uses the Win32 `ReadEventLog` API for Application and System logs
- To install as a service, run `orbis-agent.exe --install ...` **as Administrator**
- The Windows service is configured with automatic recovery actions (`SC_ACTION_RESTART × 3`)

---

## 10. Dependencies (vcpkg)

All dependencies are declared in `vcpkg.json` and installed automatically on the first `cmake` run.

| vcpkg Package | Version | Purpose |
|---|---|---|
| `paho-mqtt` | 1.3.16 | MQTT C client (synchronous API + SSL) |
| `nlohmann-json` | 3.12.0 | JSON serialization / deserialization |
| `spdlog` | 1.17.0 | Structured logging (rotating file + console) |
| `sqlite3` | 3.53.0 | Local offline buffer (WAL mode) |
| `openssl` | 3.6.2 | TLS, AES-256-CBC (token encryption), SHA-256 (update verification) |
| `curl` | 8.20.0 | Binary download for agent self-updates |
| `toml11` | 4.4.0 | `config.toml` parsing |

> Packages marked `*` in the vcpkg output (`fmt`, `zlib`, etc.) are transitive dependencies managed automatically.

### CMake Targets Used

```cmake
eclipse-paho-mqtt-c::paho-mqtt3cs-static   # Paho synchronous + SSL (static)
nlohmann_json::nlohmann_json
spdlog::spdlog
SQLite3::SQLite3
OpenSSL::SSL
OpenSSL::Crypto
CURL::libcurl
toml11::toml11
```

> **Important:** The correct vcpkg port name is `paho-mqtt` (not `eclipse-paho-mqtt-c`). The CMake `find_package` name is `eclipse-paho-mqtt-c` and the correct target is `paho-mqtt3cs-static` (synchronous + SSL), not `paho-mqtt3as-static` (async + SSL).

---

## 11. Installing as an OS Service

### Linux (systemd)

```bash
# Copy the binary and plugins
sudo cp build/orbis-agent /usr/bin/orbis-agent
sudo mkdir -p /usr/lib/orbis-agent/plugins
sudo cp build/plugins/*/*.so /usr/lib/orbis-agent/plugins/

# Set up configuration
sudo mkdir -p /etc/orbis-agent
sudo cp config.toml.example /etc/orbis-agent/config.toml
sudo nano /etc/orbis-agent/config.toml   # Set broker_host and register_secret

# Install the service (generates and enables the systemd unit)
sudo /usr/bin/orbis-agent --install --server ssl://my-broker.example.com:8883

# Check status
sudo /usr/bin/orbis-agent --status
sudo systemctl status orbis-agent
sudo journalctl -u orbis-agent -f
```

Generated systemd unit:
```ini
[Unit]
Description=Orbis Monitoring Agent
After=network-online.target

[Service]
Type=simple
ExecStart=/usr/bin/orbis-agent --config /etc/orbis-agent/config.toml
Restart=always
RestartSec=2
User=root

[Install]
WantedBy=multi-user.target
```

### macOS (launchd)

```bash
# Copy the binary and plugins
sudo cp build/orbis-agent /usr/local/bin/orbis-agent
sudo mkdir -p /usr/local/lib/orbis-agent/plugins
sudo cp build/plugins/*/*.so /usr/local/lib/orbis-agent/plugins/

# Set up configuration
sudo mkdir -p /etc/orbis-agent
sudo cp config.toml.example /etc/orbis-agent/config.toml
sudo nano /etc/orbis-agent/config.toml

# Install the service
sudo /usr/local/bin/orbis-agent --install --server ssl://my-broker.example.com:8883

# Check status
sudo /usr/local/bin/orbis-agent --status
sudo launchctl list | grep orbis
```

The generated plist is written to `/Library/LaunchDaemons/com.orbis.agent.plist` and includes `RunAtLoad` and `KeepAlive` for automatic startup on boot.

### Windows (SCM)

```powershell
# As Administrator
.\build\Release\orbis-agent.exe --install `
  --server ssl://my-broker.example.com:8883 `
  --token my_register_secret

# Check status
.\build\Release\orbis-agent.exe --status
Get-Service orbis-agent
```

The Windows service is registered with automatic recovery actions (restart after 1 second, up to 3 attempts).

---

## 12. CLI Reference

```
orbis-agent [COMMAND] [OPTIONS]

Commands:
  --install    Install and start the agent as an OS service
  --uninstall  Uninstall the OS service
  --run        Run the agent in the foreground (debug mode)
  --status     Print whether the service is running or stopped
  --version    Print the agent version

Options:
  --server  <url>   MQTT broker URL      e.g. ssl://broker.example.com:8883
  --token   <str>   Registration secret  (register_secret)
  --config  <path>  Path to config.toml  (default: platform-specific path)
```

### Examples

```bash
# Run in the foreground for debugging (logs to console + file)
./orbis-agent --run --config ./config.toml

# Install pointing to a custom broker
sudo ./orbis-agent --install \
  --server ssl://192.168.1.10:8883 \
  --token my_secret \
  --config /etc/orbis-agent/config.toml

# Uninstall the service
sudo ./orbis-agent --uninstall

# Check service status
./orbis-agent --status
```

---

## 13. Troubleshooting

### Build fails with "eclipse-paho-mqtt-c does not exist"

The vcpkg port name is `paho-mqtt`, not `eclipse-paho-mqtt-c`. Check `vcpkg.json`:
```json
"dependencies": ["paho-mqtt", ...]
```

### Undefined symbols `_MQTTClient_connect` at link time

The linked library is the **async** variant (`paho-mqtt3as`). The synchronous API (`MQTTClient.h`) requires `paho-mqtt3cs-static`. Check `CMakeLists.txt`:
```cmake
eclipse-paho-mqtt-c::paho-mqtt3cs-static
```

### `SQLite::SQLite3` deprecated warning

Use `SQLite3::SQLite3` (with the trailing `3` in both parts of the target name).

### Agent cannot connect to the broker

1. Verify `broker_host`, `broker_port`, and `register_secret` are correct in `config.toml`
2. Test network connectivity: `openssl s_client -connect broker:8883`
3. If `use_tls = true` with an empty `ca_cert_path`, certificate verification is disabled
4. Check logs: `journalctl -u orbis-agent -f` (Linux) or the `log_file` path configured in `config.toml`

### Onboarding fails (no register_ack received)

- Verify the Orbis backend is running and connected to the broker
- The topic `devices/{id}/register_ack` must be routed by the backend
- Default timeout: 30 seconds (defined in `constants.hpp` as `TIMEOUT_REGISTER_ACK_SEC`)
- The agent retries automatically every 60 seconds (`TIMEOUT_REGISTER_RETRY_SEC`)

### Plugins fail to load

1. Verify that `plugins_dir` in `config.toml` points to the correct directory containing the `.so` files directly (not subdirectories).
   - After build, plugins are at `build/plugins/cpu_ram.so`, `build/plugins/disk.so`, etc.
   - Set `plugins_dir = "./build/plugins"` in `config.test.toml` for development.
2. Plugin filenames must match exactly: `cpu_ram.so`, `disk.so`, `docker_stats.so`, `syslog_collector.so`
3. On macOS, plugins use `.so` extension (not `.dylib`) — `MODULE` targets always produce `.so` on macOS.
4. Check file permissions: `chmod +x build/plugins/*.so`

### MQTT disconnects every 1-2 seconds after startup

```
[info] MQTT connected
[warn] MQTT connection lost: unknown    ← ~1 second later
[info] MQTT reconnect attempt 1 in 1s
```

Root cause: `MQTTClient_waitForCompletion` was called from an application thread (HeartbeatService, LogCollector) while Paho's background receive thread was running. Both threads compete for Paho's internal lock → EMQX force-closes the connection.

Fix: `waitForCompletion` has been removed from `MQTTClientWrapper::publish()`. Delivery is fire-and-forget; PUBACKs are handled by the `onDeliveryComplete` callback on Paho's background thread.

If you see this after pulling a fresh build, ensure you also delete any stale SQLite WAL files (see below).

### SQLite disk I/O error on startup

```
[error] LocalBuffer: insert failed: disk I/O error
```

Cause: A previous run crashed or was force-killed, leaving orphaned WAL files that SQLite cannot recover.

Fix:
```bash
rm -f /tmp/orbis-agent-test.db /tmp/orbis-agent-test.db-wal /tmp/orbis-agent-test.db-shm
```

> Always delete all three files together — deleting only `.db` while `.db-wal` remains causes the same error on the next run.

### "Command received" logged but no result published

```
[info] Command received: <id> (type=shell)
# ... nothing after this
```

Cause (historical): the command was executed inside the Paho `onMessageArrived` callback, which blocked while waiting for a PUBACK that Paho could not deliver. Fixed by the CommandExecutor worker thread — the callback now only enqueues; the worker thread publishes results outside Paho callback context.

### `CMAKE_CXX_COMPILER not set` error on macOS

Specify the compiler explicitly:
```bash
cmake -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_CXX_COMPILER=$(xcrun -f clang++)
```

### Resetting the onboarding (re-register the device)

Clear `device_id` and `token` in `config.toml`:
```toml
[agent]
device_id = ""
token = ""
```
The agent will restart the onboarding flow on the next launch.

### rc=4 after restart — token decryption silently returns empty string

**Symptom:** The agent registered successfully and ran fine before, but fails with `rc=4` on every subsequent start without any decode error in the logs:
```
[info] Connecting MQTT as device <uuid>
[info] MQTT connecting to tcp://localhost:1883 as '<uuid>'
[error] MQTT connect failed rc=4
[critical] MQTT connect failed — aborting
```

**Root cause (fixed in current codebase):** A bug in `src/config/ConfigManager.cpp` — the `b64Decode` lookup table mapped the `=` padding character (ASCII 61) to value `0` instead of `-1`. Because the loop exits only on `lookup[c] == -1`, padding bytes were treated as data (value `0`, same as `'A'`) instead of terminators. For a token ending in `==`, this produced 2 extra null bytes, making the AES-CBC ciphertext 50 bytes instead of 48 (not a multiple of 16). `EVP_DecryptFinal_ex` rejected the padding and returned failure, so `decryptToken()` silently returned `""`. MQTT then connected with an empty password → EMQX rejected → `rc=4`.

**The fix** (one character in the lookup table, line 53):
```cpp
// BEFORE (buggy — '=' maps to 0 instead of -1):
52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,

// AFTER (correct — '=' maps to -1, breaks loop as padding):
52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
```

**If you are on an old binary**: revoke the device via the backend API, clear `device_id` and `token` in `config.toml`, delete the SQLite buffer, rebuild, and re-register.

### Restarting after a break (infrastructure was reset)

If EMQX was reinstalled or reset since the last run, its built-in database has lost all device credentials. The agent will get `rc=4` even with valid `device_id`/`token` in `config.toml`.

**Procedure:**
1. Revoke all existing devices via the backend API:
   ```bash
   TOKEN=$(curl -s -X POST http://localhost:8000/auth/login \
     -H "Content-Type: application/json" \
     -d '{"email":"admin@orbis.local","password":"change_me_in_production"}' \
     | python3 -c "import sys,json; print(json.load(sys.stdin)['access_token'])")
   # List devices to get UUIDs, then revoke each:
   curl -s -X DELETE http://localhost:8000/devices/<uuid> \
     -H "Authorization: Bearer $TOKEN"
   ```
2. Clear `config.toml`:
   ```toml
   [agent]
   device_id = ""
   token = ""
   ```
3. Delete the SQLite buffer:
   ```bash
   rm -f /tmp/orbis-agent-test.db /tmp/orbis-agent-test.db-wal /tmp/orbis-agent-test.db-shm
   ```
4. Restart the agent — it will re-register automatically.
