#pragma once
#include <mutex>
#include <string>
#include <vector>

namespace orbis {

/// @brief Typed config sub-structs returned by ConfigManager.
struct MQTTConfig {
    std::string broker_host;
    int         broker_port{8883};
    bool        use_tls{true};
    std::string ca_cert_path;
    std::string register_secret;
    int         reconnect_delay_min_sec{1};
    int         reconnect_delay_max_sec{60};
    int         keepalive_sec{60};
};

struct LogConfig {
    int                      interval_sec{60};
    std::vector<std::string> levels{"INFO","WARNING","ERROR","CRITICAL"};
    std::vector<std::string> sources;  // empty = all sources
};

struct HeartbeatConfig {
    int interval_sec{30};
};

struct PluginConfig {
    bool        enabled{true};
    std::string plugins_dir{"./plugins"};
};

struct NetworkScannerConfig {
    bool        enabled{false};
    int         scan_interval_sec{300};
    std::string port_range{"22,80,443,8080,8883"};
};

// ---------------------------------------------------------------------------

/**
 * @brief Thread-safe singleton managing the agent configuration.
 *
 * Loads config.toml on startup, persists device_id and token after onboarding,
 * and optionally reloads on SIGHUP (Linux/macOS).
 * The token is stored AES-256 encrypted on disk; it is decrypted transparently
 * when getToken() is called.
 */
class ConfigManager {
public:
    static ConfigManager& instance();

    // Non-copyable, non-movable (singleton)
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    /// Load configuration from the given TOML file path.
    /// @throws std::runtime_error if the file cannot be parsed.
    bool load(const std::string& config_path);

    /// Persist the current in-memory config back to the file.
    bool save();

    /// @name Agent identity
    /// @{
    std::string getDeviceId() const;
    void        setDeviceId(const std::string& id);
    /// Returns the plaintext token (decrypts from disk representation).
    std::string getToken() const;
    /// Stores the token (encrypts before writing to disk via save()).
    void        setToken(const std::string& token);
    bool        isRegistered() const;
    /// @}

    /// @name Config sub-structs
    /// @{
    MQTTConfig          getMQTTConfig()          const;
    LogConfig           getLogConfig()           const;
    HeartbeatConfig     getHeartbeatConfig()     const;
    PluginConfig        getPluginConfig()        const;
    NetworkScannerConfig getNetworkScannerConfig() const;
    std::string         getLogFilePath()         const;
    std::string         getBufferDbPath()        const;
    /// @}

private:
    ConfigManager() = default;

    /// Encrypt plaintext with AES-256-CBC, key derived from hostname.
    static std::string encryptToken(const std::string& plaintext);
    /// Decrypt ciphertext produced by encryptToken.
    static std::string decryptToken(const std::string& ciphertext);
    /// Derive a 32-byte key from the machine hostname (SHA-256).
    static std::vector<unsigned char> deriveKey();

    mutable std::mutex mu_;

    std::string config_path_;

    // [agent]
    std::string device_id_;
    std::string token_encrypted_;  // AES-256 ciphertext, base64-encoded on disk
    std::string log_file_;
    std::string buffer_db_;

    // [mqtt]
    MQTTConfig mqtt_;

    // [logs]
    LogConfig log_;

    // [heartbeat]
    HeartbeatConfig heartbeat_;

    // [plugins]
    PluginConfig plugins_;

    // [network_scanner]
    NetworkScannerConfig scanner_;
};

} // namespace orbis
