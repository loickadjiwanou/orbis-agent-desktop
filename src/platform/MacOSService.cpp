#include "ServiceInstaller.hpp"
#include "constants.hpp"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace orbis {

class MacOSService final : public ServiceInstaller {
public:
    bool install(const std::string& exec_path,
                 const std::string& config_path) override
    {
        // 1. Create config directory
        std::error_code ec;
        fs::create_directories("/etc/orbis-agent", ec);

        // 2. Copy binary to /usr/local/bin
        const std::string dest_bin = constants::BINARY_INSTALL_PATH;
        fs::copy_file(exec_path, dest_bin, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            spdlog::error("MacOSService: cannot copy binary: {}", ec.message());
            return false;
        }
        fs::permissions(dest_bin,
                        fs::perms::owner_all | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::replace, ec);

        // 3. Copy config if not present
        const std::string dest_cfg = "/etc/orbis-agent/config.toml";
        if (!fs::exists(dest_cfg) && !config_path.empty() && fs::exists(config_path)) {
            fs::copy_file(config_path, dest_cfg, ec);
        }

        // 4. Write launchd plist
        std::ofstream plist(constants::LAUNCHD_PLIST_PATH);
        if (!plist) {
            spdlog::error("MacOSService: cannot write plist at {}", constants::LAUNCHD_PLIST_PATH);
            return false;
        }
        plist <<
            R"(<?xml version="1.0" encoding="UTF-8"?>)" "\n"
            R"(<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN")" "\n"
            R"(  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">)" "\n"
            R"(<plist version="1.0">)" "\n"
            "<dict>\n"
            "  <key>Label</key><string>" << constants::LAUNCHD_LABEL << "</string>\n"
            "  <key>ProgramArguments</key>\n"
            "  <array>\n"
            "    <string>" << dest_bin << "</string>\n"
            "    <string>--config</string>\n"
            "    <string>" << dest_cfg << "</string>\n"
            "  </array>\n"
            "  <key>RunAtLoad</key><true/>\n"
            "  <key>KeepAlive</key><true/>\n"
            "  <key>StandardOutPath</key><string>/var/log/orbis-agent.log</string>\n"
            "  <key>StandardErrorPath</key><string>/var/log/orbis-agent-error.log</string>\n"
            "</dict>\n"
            "</plist>\n";
        plist.close();

        // 5. launchctl load
        std::string load_cmd = std::string("launchctl load -w ") + constants::LAUNCHD_PLIST_PATH;
        if (system(load_cmd.c_str()) != 0) {
            spdlog::error("MacOSService: launchctl load failed");
            return false;
        }

        spdlog::info("MacOSService: {} installed and started", constants::LAUNCHD_LABEL);
        return true;
    }

    bool uninstall() override {
        stop();
        std::string unload = std::string("launchctl unload -w ") + constants::LAUNCHD_PLIST_PATH;
        system(unload.c_str());
        fs::remove(constants::LAUNCHD_PLIST_PATH);
        spdlog::info("MacOSService: {} uninstalled", constants::LAUNCHD_LABEL);
        return true;
    }

    bool start() override {
        std::string cmd = std::string("launchctl start ") + constants::LAUNCHD_LABEL;
        return system(cmd.c_str()) == 0;
    }

    bool stop() override {
        std::string cmd = std::string("launchctl stop ") + constants::LAUNCHD_LABEL;
        return system(cmd.c_str()) == 0;
    }

    bool isRunning() override {
        std::string cmd = std::string("launchctl list ") + constants::LAUNCHD_LABEL +
                          " > /dev/null 2>&1";
        return system(cmd.c_str()) == 0;
    }
};

std::unique_ptr<ServiceInstaller> ServiceInstaller::create() {
    return std::make_unique<MacOSService>();
}

} // namespace orbis
