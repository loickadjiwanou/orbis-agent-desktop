#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

namespace orbis {

/**
 * @brief Publishes a heartbeat (status) message every N seconds.
 *
 * Runs in its own named thread.  The payload includes all system metrics
 * (CPU %, RAM %, disk %) collected directly via OS APIs without external deps.
 * On clean shutdown publishes a final "offline" status.
 *
 * Section 3 — SESSION_0_CONTRAT_INTERFACES:
 *   topic   : devices/{device_id}/status
 *   QoS     : 1
 *   Retained: true
 */
class HeartbeatService {
public:
    /**
     * @param publish_fn  Called to publish each heartbeat JSON.
     *                    Returns true on success.
     * @param device_id   The agent's registered device UUID.
     * @param interval_sec  Heartbeat interval (default 30s, Section 14).
     */
    HeartbeatService(std::function<bool(const std::string& topic,
                                        const std::string& payload,
                                        int qos, bool retained)> publish_fn,
                     const std::string& device_id,
                     int interval_sec);
    ~HeartbeatService();

    void start();
    void stop();
    bool isRunning() const;

private:
    void run();
    std::string buildPayload(const std::string& status) const;

    // Platform-specific system metrics
    static double getCpuPercent();
    static double getRamPercent();
    static double getDiskPercent(const std::string& path = "/");
    static std::string getHostname();
    static std::string getPlatform();
    static std::string getOsVersion();
    static std::string getArchitecture();
    static long long   getUptimeSec();
    // Returns battery level 0-100, or -1 if no battery / not supported
    static int    getBatteryLevel();
    // Returns true if charging, false if on battery or unknown
    static bool   isBatteryCharging();
    // Returns "WIFI", "ETHERNET", or "NONE"
    static std::string getNetworkType();

    std::function<bool(const std::string&, const std::string&, int, bool)> publish_fn_;
    std::string device_id_;
    int         interval_sec_;

    std::thread      thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
};

} // namespace orbis
