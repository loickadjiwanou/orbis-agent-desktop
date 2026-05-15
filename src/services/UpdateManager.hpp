#pragma once
#include <functional>
#include <string>

namespace orbis {

/**
 * @brief Handles agent self-update via the agent_update command.
 *
 * Workflow (Section 7 — SESSION_0_CONTRAT_INTERFACES):
 *  1. Receive payload: { url, sha256, version, command_id }
 *  2. Publish "executing"
 *  3. Backup current binary → orbis-agent.bak
 *  4. Download new binary via libcurl
 *  5. Verify SHA-256 (OpenSSL)
 *  6. Replace binary atomically
 *  7. Restart the service
 *  8. Publish "success" or "failed" (with rollback on failure)
 */
class UpdateManager {
public:
    UpdateManager(
        std::function<bool(const std::string& topic,
                           const std::string& payload)> publish_fn,
        const std::string& device_id);

    /**
     * @brief Execute the update. Blocking call — run in a dedicated thread.
     * @param command_id  MQTT command_id for result publishing.
     * @param url         Download URL for the new binary.
     * @param sha256      Expected SHA-256 hex digest.
     * @param version     Target version string (for logging).
     */
    void update(const std::string& command_id,
                const std::string& url,
                const std::string& sha256,
                const std::string& version);

private:
    void publishResult(const std::string& command_id,
                       const std::string& statut,
                       const std::string& output,
                       const std::string& error);

    void publishProgress(const std::string& command_id,
                         const std::string& step,
                         const std::string& output = "",
                         const std::string& error  = "");

    bool downloadBinary(const std::string& url, const std::string& dest_path);
    bool verifySHA256(const std::string& file_path, const std::string& expected_hex);
    bool replaceBinary(const std::string& new_path);
    bool restartService();
    bool rollback(const std::string& backup_path);

    static std::string currentBinaryPath();
    static std::string nowISO8601();

    std::function<bool(const std::string&, const std::string&)> publish_fn_;
    std::string device_id_;
};

} // namespace orbis
