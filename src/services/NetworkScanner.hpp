#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace orbis {

/**
 * @brief Optional network scanner — scans the local subnet and publishes
 * discovered neighbors to devices/{device_id}/discovery.
 *
 * Section 8 — SESSION_0_CONTRAT_INTERFACES:
 *   topic     : devices/{device_id}/discovery
 *   QoS       : 1
 *   Retained  : false
 *
 * Uses cross-platform TCP connect() with non-blocking sockets.
 * Only active when enabled = true in config.
 */
class NetworkScanner {
public:
    NetworkScanner(
        std::function<bool(const std::string& topic,
                           const std::string& payload)> publish_fn,
        const std::string& device_id,
        int  scan_interval_sec,
        const std::string& port_range);
    ~NetworkScanner();

    void start();
    void stop();
    bool isRunning() const;

    /// Trigger an immediate scan (e.g. on "scan_network" command).
    void triggerNow();

private:
    struct Neighbor {
        std::string         ip;
        std::string         hostname;
        std::vector<int>    open_ports;
        int                 response_time_ms{0};
    };

    void run();
    void scan();

    std::string         detectLocalSubnet();
    std::vector<std::string> expandCIDR(const std::string& cidr);
    bool                pingHost(const std::string& ip, int& response_ms);
    bool                probePort(const std::string& ip, int port, int timeout_ms = 500);
    std::string         resolveHostname(const std::string& ip);
    std::vector<int>    parsePorts(const std::string& port_range);
    static std::string  nowISO8601();

    std::function<bool(const std::string&, const std::string&)> publish_fn_;
    std::string device_id_;
    int         scan_interval_sec_;
    std::string port_range_;

    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> trigger_now_{false};
};

} // namespace orbis
