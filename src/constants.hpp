#pragma once
#include <string>

/// @file constants.hpp
/// @brief All compile-time and runtime constants for the Orbis agent.
/// No magic literals anywhere else in the codebase — everything goes here.

namespace orbis::constants {

// ---------------------------------------------------------------------------
// Agent identity
// ---------------------------------------------------------------------------
inline constexpr const char *AGENT_VERSION = "1.0.0";
inline constexpr const char *AGENT_NAME = "orbis-agent";

// ---------------------------------------------------------------------------
// MQTT topic templates (use fmt::format or topicFor() to substitute device_id)
// ---------------------------------------------------------------------------
inline constexpr const char *TOPIC_REGISTER = "devices/register";
/// After substitution: "devices/{device_id}/register_ack"
inline constexpr const char *TOPIC_REGISTER_ACK_FMT = "devices/{}/register_ack";
inline constexpr const char *TOPIC_LOGS_FMT = "devices/{}/logs";
inline constexpr const char *TOPIC_STATUS_FMT = "devices/{}/status";
inline constexpr const char *TOPIC_COMMANDS_FMT = "devices/{}/commands";
inline constexpr const char *TOPIC_RESULTS_FMT = "devices/{}/results";
inline constexpr const char *TOPIC_UPDATE_FMT = "devices/{}/update";
inline constexpr const char *TOPIC_UPDATE_PROGRESS_FMT =
    "devices/{}/update_progress";
inline constexpr const char *TOPIC_DISCOVERY_FMT = "devices/{}/discovery";

/// Substitute a single {} placeholder with device_id.
inline std::string topicFor(const char *fmt, const std::string &device_id) {
  std::string t(fmt);
  auto pos = t.find("{}");
  if (pos != std::string::npos)
    t.replace(pos, 2, device_id);
  return t;
}

// ---------------------------------------------------------------------------
// MQTT connection defaults
// ---------------------------------------------------------------------------
inline constexpr const char *MQTT_REGISTER_USERNAME = "register";
inline constexpr int MQTT_DEFAULT_QOS = 1;
inline constexpr int MQTT_KEEPALIVE_SEC = 60;
inline constexpr int MQTT_RECONNECT_MIN_SEC = 1;
inline constexpr int MQTT_RECONNECT_MAX_SEC = 60;

// ---------------------------------------------------------------------------
// Timeouts (seconds) — Section 14 of SESSION_0_CONTRAT_INTERFACES
// ---------------------------------------------------------------------------
inline constexpr int TIMEOUT_REGISTER_ACK_SEC = 30;
inline constexpr int TIMEOUT_REGISTER_RETRY_SEC = 60;
inline constexpr int TIMEOUT_COMMAND_DEFAULT_SEC = 30;
inline constexpr int TIMEOUT_AGENT_UPDATE_SEC = 300;
inline constexpr int TIMEOUT_DEVICE_INACTIVITY_SEC = 300;

// ---------------------------------------------------------------------------
// Collection intervals — Section 14
// ---------------------------------------------------------------------------
inline constexpr int INTERVAL_HEARTBEAT_SEC = 30;
inline constexpr int INTERVAL_LOG_COLLECT_SEC = 60;
inline constexpr int INTERVAL_NETWORK_SCAN_SEC = 300;
inline constexpr int INTERVAL_WATCHDOG_SEC = 5;
inline constexpr int INTERVAL_BUFFER_FLUSH_SEC = 10;

// ---------------------------------------------------------------------------
// Buffer / size limits — Section 14
// ---------------------------------------------------------------------------
inline constexpr int BUFFER_MAX_MESSAGES = 100000;
inline constexpr int BUFFER_FLUSH_BATCH_SIZE = 50;
inline constexpr int LOG_BATCH_MAX = 100;
inline constexpr int COMMAND_OUTPUT_MAX_BYTES = 65536; // 64 KB
inline constexpr int MQTT_MESSAGE_MAX_BYTES = 1048576; // 1 MB

// ---------------------------------------------------------------------------
// Platform-specific config paths
// ---------------------------------------------------------------------------
#if defined(PLATFORM_WINDOWS)
inline constexpr const char *DEFAULT_CONFIG_PATH =
    R"(C:\ProgramData\Orbis\config.toml)";
inline constexpr const char *DEFAULT_LOG_PATH =
    R"(C:\ProgramData\Orbis\agent.log)";
inline constexpr const char *DEFAULT_BUFFER_PATH =
    R"(C:\ProgramData\Orbis\buffer.db)";
inline constexpr const char *DEFAULT_PLUGINS_DIR =
    R"(C:\Program Files\Orbis\plugins)";
inline constexpr const char *BINARY_INSTALL_PATH =
    R"(C:\Program Files\Orbis\orbis-agent.exe)";
#else
inline constexpr const char *DEFAULT_CONFIG_PATH =
    "/etc/orbis-agent/config.toml";
inline constexpr const char *DEFAULT_LOG_PATH = "/var/log/orbis-agent.log";
inline constexpr const char *DEFAULT_BUFFER_PATH =
    "/var/lib/orbis-agent/buffer.db";
inline constexpr const char *DEFAULT_PLUGINS_DIR =
    "/usr/lib/orbis-agent/plugins";
inline constexpr const char *BINARY_INSTALL_PATH = "/usr/bin/orbis-agent";
#endif

// ---------------------------------------------------------------------------
// Uninstall protection key file paths
// ---------------------------------------------------------------------------
#if defined(PLATFORM_WINDOWS)
inline constexpr const char *UNINSTALL_KEY_PATH =
    R"(C:\ProgramData\Orbis\.uninstall_key)";
#elif defined(PLATFORM_MACOS)
inline constexpr const char *UNINSTALL_KEY_PATH =
    "/etc/orbis-agent/.uninstall_key";
#else
inline constexpr const char *UNINSTALL_KEY_PATH =
    "/etc/orbis-agent/.uninstall_key";
#endif

// Default uninstall code used when --uninstall-code is not provided at install
// time
inline constexpr const char *DEFAULT_UNINSTALL_CODE = "orbis2026";

// ---------------------------------------------------------------------------
// Service names
// ---------------------------------------------------------------------------
inline constexpr const char *SERVICE_NAME = "orbis-agent";
inline constexpr const char *SERVICE_DISPLAY_NAME = "Orbis Monitoring Agent";
inline constexpr const char *SERVICE_DESCRIPTION =
    "Orbis remote monitoring and management agent";
inline constexpr const char *LAUNCHD_LABEL = "com.orbis.agent";
inline constexpr const char *LAUNCHD_PLIST_PATH =
    "/Library/LaunchDaemons/com.orbis.agent.plist";
inline constexpr const char *SYSTEMD_UNIT_PATH =
    "/etc/systemd/system/orbis-agent.service";

} // namespace orbis::constants
