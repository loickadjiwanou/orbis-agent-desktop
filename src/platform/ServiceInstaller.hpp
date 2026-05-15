#pragma once
#include <memory>
#include <string>

namespace orbis {

/**
 * @brief Abstract interface for OS-level service management.
 *
 * Concrete implementations: LinuxService (systemd), WindowsService (SCM),
 * MacOSService (launchd).  Use the factory method create() to get the
 * correct implementation for the current platform at runtime.
 */
class ServiceInstaller {
public:
    virtual ~ServiceInstaller() = default;

    /**
     * @brief Install the agent as a system service and start it.
     * @param exec_path   Absolute path to the orbis-agent binary.
     * @param config_path Absolute path to config.toml.
     * @return true on success.
     */
    virtual bool install(const std::string& exec_path,
                         const std::string& config_path) = 0;

    /// Stop and remove the service. Does not delete the binary or config.
    virtual bool uninstall() = 0;

    /// Start a previously installed service.
    virtual bool start() = 0;

    /// Stop the running service.
    virtual bool stop() = 0;

    /// Returns true if the service is currently running.
    virtual bool isRunning() = 0;

    /**
     * @brief Factory method — returns the correct implementation for the
     * current OS at runtime.
     */
    static std::unique_ptr<ServiceInstaller> create();
};

} // namespace orbis
