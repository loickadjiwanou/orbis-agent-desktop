#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace orbis {

class PluginLoader;

/**
 * @brief Collects log entries from plugins and native OS log sources,
 * batches them, and publishes to devices/{device_id}/logs.
 *
 * Runs in its own named thread.
 * If MQTT is unavailable, delegates to LocalBuffer via store_fn.
 *
 * Section 4 — SESSION_0_CONTRAT_INTERFACES:
 *   topic     : devices/{device_id}/logs
 *   QoS       : 1
 *   Retained  : false
 *   Batch max : 100 logs per message
 */
class LogCollector {
public:
    /**
     * @param publish_fn   Publishes a batch JSON to MQTT. Returns true on success.
     * @param store_fn     Called when MQTT is unavailable; stores to LocalBuffer.
     * @param plugin_loader Already-initialized PluginLoader (may be nullptr).
     * @param device_id    The agent's device UUID.
     * @param interval_sec Log collection interval (default 60s).
     * @param log_levels   Set of accepted log levels (e.g. {"INFO","ERROR"}).
     */
    LogCollector(std::function<bool(const std::string& topic,
                                    const std::string& payload)>   publish_fn,
                 std::function<void(const std::string& topic,
                                    const std::string& payload)>   store_fn,
                 PluginLoader*                                      plugin_loader,
                 const std::string&                                device_id,
                 int                                               interval_sec,
                 std::vector<std::string>                          log_levels);
    ~LogCollector();

    void start();
    void stop();
    bool isRunning() const;

    /// Trigger an immediate collection cycle (e.g. on "collect_now" command).
    void triggerNow();

private:
    void run();
    void collectAndPublish();

    struct LogEntry {
        std::string device_id;
        std::string timestamp;
        std::string level;
        std::string source;
        std::string message;
        std::string metadata_json;
    };

    std::vector<LogEntry> collectFromPlugins();
    std::vector<LogEntry> collectFromSystem();
    bool levelAllowed(const std::string& level) const;
    static std::string nowISO8601();

    std::function<bool(const std::string&, const std::string&)> publish_fn_;
    std::function<void(const std::string&, const std::string&)> store_fn_;
    PluginLoader*  plugin_loader_;
    std::string    device_id_;
    int            interval_sec_;
    std::vector<std::string> log_levels_;

    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> trigger_now_{false};
};

} // namespace orbis
