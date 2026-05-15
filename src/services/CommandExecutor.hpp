#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace orbis {

class LogCollector;
class NetworkScanner;
class UpdateManager;

/**
 * @brief Subscribes to devices/{device_id}/commands, executes each command,
 * and publishes lifecycle results to devices/{device_id}/results.
 *
 * Command lifecycle per Section 6 of SESSION_0_CONTRAT_INTERFACES:
 *   acknowledged → executing → success | failed
 *
 * Runs in its own named thread processing commands from the MessageQueue.
 * All result publishes use the exact field names from Session 0:
 *   command_id, device_id, statut, output, error, exit_code, timestamp
 */
class CommandExecutor {
public:
    /**
     * @param publish_fn    Publish a result message. Returns true on success.
     * @param subscribe_fn  Subscribe to a topic (called once at start).
     * @param msg_callback  Register the inbound message callback on MQTTClient.
     * @param device_id     Agent device UUID.
     * @param log_collector For "collect_now" command delegation (may be nullptr).
     * @param network_scanner For "scan_network" delegation (may be nullptr).
     */
    CommandExecutor(
        std::function<bool(const std::string& topic, const std::string& payload)> publish_fn,
        std::function<bool(const std::string& topic)>                             subscribe_fn,
        std::function<void(std::function<void(const std::string&,
                                              const std::string&)>)>              set_msg_cb,
        const std::string& device_id,
        LogCollector*      log_collector,
        NetworkScanner*    network_scanner,
        UpdateManager*     update_manager = nullptr);
    ~CommandExecutor();

    void start();
    void stop();
    bool isRunning() const;

private:
    // Called from MQTT callback — only enqueues, never blocks.
    void processCommand(const std::string& topic, const std::string& payload);

    // Executed by the worker thread — safe to call publish here.
    void executeCommand(const std::string& payload);

    void workerLoop();

    void publishResult(const std::string& command_id,
                       const std::string& statut,
                       const std::string& output,
                       const std::string& error,
                       int exit_code);

    // Command handlers
    void handleShell(const std::string& cmd_id, const std::string& payload_str, int timeout_sec);
    void handleRestart(const std::string& cmd_id);
    void handleCollectNow(const std::string& cmd_id);
    void handleScanNetwork(const std::string& cmd_id);
    void handleGetInfo(const std::string& cmd_id);

    /// Run a shell command, capture stdout+stderr, respect timeout.
    struct ShellResult { int exit_code; std::string output; std::string error; };
    static ShellResult runShell(const std::string& command, int timeout_sec);

    static std::string nowISO8601();

    std::function<bool(const std::string&, const std::string&)> publish_fn_;
    std::function<bool(const std::string&)>                     subscribe_fn_;
    std::string device_id_;
    LogCollector*   log_collector_;
    NetworkScanner* network_scanner_;
    UpdateManager*  update_manager_;

    // Worker thread + command queue (decouples MQTT callback from publish)
    std::thread              worker_thread_;
    std::queue<std::string>  command_queue_;
    std::mutex               queue_mutex_;
    std::condition_variable  queue_cv_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
};

} // namespace orbis
