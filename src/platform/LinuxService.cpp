#include "ServiceInstaller.hpp"
#include "constants.hpp"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace orbis {

// ---------------------------------------------------------------------------
// Linux (systemd) implementation
// ---------------------------------------------------------------------------

class LinuxService final : public ServiceInstaller {
public:
    bool install(const std::string& exec_path,
                 const std::string& config_path) override
    {
        // 1. Create config directory
        const std::string config_dir = "/etc/orbis-agent";
        std::error_code ec;
        fs::create_directories(config_dir, ec);
        if (ec) {
            spdlog::error("LinuxService: cannot create {}: {}", config_dir, ec.message());
            return false;
        }

        // 2. Copy binary to /usr/bin/orbis-agent
        const std::string dest_bin = constants::BINARY_INSTALL_PATH;
        fs::copy_file(exec_path, dest_bin, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            spdlog::error("LinuxService: cannot copy binary to {}: {}", dest_bin, ec.message());
            return false;
        }
        fs::permissions(dest_bin,
                        fs::perms::owner_all | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::replace, ec);

        // 3. Copy config if not already present
        const std::string dest_cfg = std::string(config_dir) + "/config.toml";
        if (!fs::exists(dest_cfg)) {
            if (!config_path.empty() && fs::exists(config_path)) {
                fs::copy_file(config_path, dest_cfg, ec);
            } else {
                // Write a minimal default config
                std::ofstream f(dest_cfg);
                f << "# Orbis Agent config — edit before starting\n"
                  << "[agent]\ndevice_id = \"\"\ntoken = \"\"\n"
                  << "[mqtt]\nbroker_host = \"localhost\"\nbroker_port = 8883\nuse_tls = true\n"
                  << "[heartbeat]\ninterval_sec = 30\n"
                  << "[logs]\ninterval_sec = 60\n";
            }
        }

        // 4. Write systemd unit file
        const std::string unit = std::string(constants::SYSTEMD_UNIT_PATH);
        std::ofstream uf(unit);
        if (!uf) {
            spdlog::error("LinuxService: cannot write {}", unit);
            return false;
        }
        uf << "[Unit]\n"
           << "Description=" << constants::SERVICE_DESCRIPTION << "\n"
           << "After=network.target\n"
           << "StartLimitIntervalSec=0\n\n"
           << "[Service]\n"
           << "Type=simple\n"
           << "ExecStart=" << dest_bin << " --config " << dest_cfg << "\n"
           << "Restart=always\n"
           << "RestartSec=2\n"
           << "StartLimitBurst=0\n"
           << "User=root\n"
           << "StandardOutput=journal\n"
           << "StandardError=journal\n"
           << "SyslogIdentifier=" << constants::SERVICE_NAME << "\n\n"
           << "[Install]\n"
           << "WantedBy=multi-user.target\n";
        uf.close();

        // 5. systemctl daemon-reload, enable, start
        if (system("systemctl daemon-reload") != 0) {
            spdlog::warn("LinuxService: daemon-reload failed (non-fatal)");
        }
        std::string enable_cmd = std::string("systemctl enable ") + constants::SERVICE_NAME;
        std::string start_cmd  = std::string("systemctl start ")  + constants::SERVICE_NAME;

        if (system(enable_cmd.c_str()) != 0) {
            spdlog::error("LinuxService: systemctl enable failed");
            return false;
        }
        if (system(start_cmd.c_str()) != 0) {
            spdlog::error("LinuxService: systemctl start failed");
            return false;
        }

        spdlog::info("LinuxService: {} installed and started", constants::SERVICE_NAME);
        return true;
    }

    bool uninstall() override {
        stop();
        std::string disable_cmd = std::string("systemctl disable ") + constants::SERVICE_NAME;
        (void)system(disable_cmd.c_str());
        fs::remove(constants::SYSTEMD_UNIT_PATH);
        (void)system("systemctl daemon-reload");
        spdlog::info("LinuxService: {} uninstalled", constants::SERVICE_NAME);
        return true;
    }

    bool start() override {
        std::string cmd = std::string("systemctl start ") + constants::SERVICE_NAME;
        return system(cmd.c_str()) == 0;
    }

    bool stop() override {
        std::string cmd = std::string("systemctl stop ") + constants::SERVICE_NAME;
        return system(cmd.c_str()) == 0;
    }

    bool isRunning() override {
        std::string cmd = std::string("systemctl is-active --quiet ") + constants::SERVICE_NAME;
        return system(cmd.c_str()) == 0;
    }
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<ServiceInstaller> ServiceInstaller::create() {
    return std::make_unique<LinuxService>();
}

} // namespace orbis
