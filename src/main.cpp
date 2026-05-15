/**
 * @file main.cpp
 * @brief Orbis Agent — point d'entrée principal.
 *
 * CLI:
 *   --install  --server <url> [--token <reg_secret>] [--config <path>]
 *   --uninstall
 *   --run      [--config <path>]   ← foreground / debug
 *   --version
 *   --status
 *
 * Flux normal (lancé par le service OS) :
 *   1. Charger config
 *   2. Initialiser spdlog
 *   3. Initialiser LocalBuffer (SQLite)
 *   4. Connecter MQTT (credentials temporaires "register" si non inscrit)
 *   5. Onboarding si device_id absent
 *   6. Connecter MQTT avec credentials définitifs
 *   7. Démarrer tous les services
 *   8. Watchdog loop (5 s)
 *   9. SIGINT/SIGTERM → arrêt propre
 *
 * Note Session 0 (priorité absolue) :
 *   - register_ack topic : devices/{device_id}/register_ack
 *   - Onboarding username : "register", password : register_secret
 */
#include "constants.hpp"
#include "config/ConfigManager.hpp"
#include "mqtt/MQTTClient.hpp"
#include "core/MessageQueue.hpp"
#include "buffer/LocalBuffer.hpp"
#include "plugins/PluginLoader.hpp"
#include "services/HeartbeatService.hpp"
#include "services/LogCollector.hpp"
#include "services/CommandExecutor.hpp"
#include "services/UpdateManager.hpp"
#include "services/NetworkScanner.hpp"
#include "platform/ServiceInstaller.hpp"
#include "mqtt/MqttLogSink.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <fstream>
#include <iomanip>
#include <sstream>

#include <openssl/evp.h>

#if !defined(PLATFORM_WINDOWS)
#  include <unistd.h>
#  include <sys/stat.h>
#  include <sys/utsname.h>
#endif
#if defined(PLATFORM_MACOS)
#  include <sys/sysctl.h>
#  include <mach-o/dyld.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;
namespace constants = orbis::constants;

// ---------------------------------------------------------------------------
// Global shutdown flag
// ---------------------------------------------------------------------------
static std::atomic<bool> g_shutdown{false};
static std::mutex        g_shutdown_mutex;
static std::condition_variable g_shutdown_cv;

static void handleSignal(int /*sig*/) {
    g_shutdown = true;
    g_shutdown_cv.notify_all();
}

// ---------------------------------------------------------------------------
// Uninstall protection helpers
// ---------------------------------------------------------------------------

/// Compute SHA-256 of `input` and return as hex string.
static std::string sha256hex(const std::string& input) {
    unsigned char digest[EVP_MAX_MD_SIZE] = {};
    unsigned int  digest_len = 0;
    EVP_MD_CTX*   ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, input.data(), input.size());
    EVP_DigestFinal_ex(ctx, digest, &digest_len);
    EVP_MD_CTX_free(ctx);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digest_len; ++i)
        oss << std::setw(2) << static_cast<int>(digest[i]);
    return oss.str();
}

/// Persist the SHA-256 hash of `code` into UNINSTALL_KEY_PATH.
/// The file is created with restricted permissions (root-only readable).
static bool saveUninstallKey(const std::string& code) {
    const std::string key_path = constants::UNINSTALL_KEY_PATH;
    fs::create_directories(fs::path(key_path).parent_path());

    std::ofstream f(key_path, std::ios::trunc);
    if (!f) {
        std::cerr << "Cannot write uninstall key to " << key_path << "\n";
        return false;
    }
    f << sha256hex(code) << "\n";
    f.close();

#if !defined(PLATFORM_WINDOWS)
    // Restrict to root read-only
    chmod(key_path.c_str(), 0600);
#endif
    return true;
}

/// Return true if `code` matches the stored uninstall key hash.
static bool verifyUninstallCode(const std::string& code) {
    const std::string key_path = constants::UNINSTALL_KEY_PATH;
    std::ifstream f(key_path);
    if (!f) return false;   // no key file → no protection set, allow

    std::string stored_hash;
    std::getline(f, stored_hash);
    return !stored_hash.empty() && (sha256hex(code) == stored_hash);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::string nowISO8601() {
    auto now   = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now_t));
    return buf;
}

static std::string getHostname() {
    char buf[256] = {};
#if defined(PLATFORM_WINDOWS)
    DWORD len = sizeof(buf); GetComputerNameA(buf, &len);
#else
    gethostname(buf, sizeof(buf) - 1);
#endif
    return buf;
}

static std::string getPlatform() {
#if defined(PLATFORM_WINDOWS)
    return "windows";
#elif defined(PLATFORM_MACOS)
    return "macos";
#else
    return "linux";
#endif
}

static std::string getOsVersion() {
#if defined(PLATFORM_LINUX)
    std::ifstream f("/etc/os-release");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            auto v = line.substr(12);
            if (!v.empty() && v.front() == '"') v = v.substr(1);
            if (!v.empty() && v.back()  == '"') v.pop_back();
            return v;
        }
    }
    return "Linux";
#elif defined(PLATFORM_MACOS)
    char buf[64] = {}; size_t len = sizeof(buf);
    sysctlbyname("kern.osproductversion", buf, &len, nullptr, 0);
    return std::string("macOS ") + buf;
#elif defined(PLATFORM_WINDOWS)
    return "Windows";
#else
    return "Unknown";
#endif
}

static std::string getArchitecture() {
#if defined(PLATFORM_WINDOWS)
    SYSTEM_INFO si{}; GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: return "x86_64";
        case PROCESSOR_ARCHITECTURE_ARM64: return "arm64";
        default:                           return "x86";
    }
#else
    struct utsname u{}; uname(&u); return u.machine;
#endif
}

// ---------------------------------------------------------------------------
// Initialise spdlog
// ---------------------------------------------------------------------------
static void initLogging(const std::string& log_file, bool foreground) {
    std::vector<spdlog::sink_ptr> sinks;

    // Always log to rotating file (10 MB × 3 files)
    try {
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_file, 10 * 1024 * 1024, 3));
    } catch (const std::exception& e) {
        std::cerr << "[WARN] Cannot open log file " << log_file << ": " << e.what() << "\n";
    }

    // Console sink in foreground mode
    if (foreground) {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }

    auto logger = std::make_shared<spdlog::logger>(
        "orbis", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::debug);
    logger->set_pattern("[%Y-%m-%dT%H:%M:%SZ] [%l] %v");
    spdlog::set_default_logger(logger);
    spdlog::flush_on(spdlog::level::info);
}

// ---------------------------------------------------------------------------
// Onboarding — Session 0, Section 2
// ---------------------------------------------------------------------------
static bool doOnboarding(orbis::MQTTClientWrapper& mqtt,
                          const std::string& device_id,
                          const std::string& register_secret)
{
    spdlog::info("Onboarding: connecting as 'register' user");

    // Connect with temporary "register" credentials (Session 0, Section 2, Étape 2)
    bool ok = mqtt.connect(constants::MQTT_REGISTER_USERNAME,
                           register_secret,
                           /*will_topic=*/"",
                           /*will_payload=*/"",
                           constants::MQTT_KEEPALIVE_SEC);
    if (!ok) {
        spdlog::error("Onboarding: cannot connect with register credentials");
        return false;
    }

    // Subscribe to register_ack BEFORE publishing (Session 0, Section 2)
    std::string ack_topic = constants::topicFor(constants::TOPIC_REGISTER_ACK_FMT, device_id);
    mqtt.subscribe(ack_topic, constants::MQTT_DEFAULT_QOS);

    // Prepare register payload (Session 0, Section 2, Étape 3)
    json reg;
    reg["device_id"]     = device_id;
    reg["hostname"]      = getHostname();
    reg["plateforme"]    = getPlatform();
    reg["os_version"]    = getOsVersion();
    reg["architecture"]  = getArchitecture();
    reg["version_agent"] = constants::AGENT_VERSION;
    reg["timestamp"]     = nowISO8601();

    // Shared state for the ack callback
    std::string received_token;
    bool        ack_received = false;
    std::mutex  ack_mutex;
    std::condition_variable ack_cv;

    mqtt.setMessageCallback([&](const std::string& topic, const std::string& payload) {
        if (topic != ack_topic) return;
        try {
            auto j = json::parse(payload);
            std::lock_guard<std::mutex> lock(ack_mutex);
            if (j.value("status", "") == "ok") {
                received_token = j.value("token", "");
                ack_received   = true;
            } else {
                spdlog::error("Onboarding rejected: {}", j.value("message", "unknown"));
                ack_received = true; // unblock, token stays empty
            }
            ack_cv.notify_all();
        } catch (const std::exception& e) {
            spdlog::error("Onboarding: bad ack JSON: {}", e.what());
        }
    });

    // Publish devices/register
    spdlog::info("Onboarding: publishing to devices/register");
    if (!mqtt.publish(constants::TOPIC_REGISTER, reg.dump(),
                      constants::MQTT_DEFAULT_QOS, false)) {
        spdlog::error("Onboarding: publish failed");
        return false;
    }

    // Wait up to TIMEOUT_REGISTER_ACK_SEC seconds for register_ack
    {
        std::unique_lock<std::mutex> lock(ack_mutex);
        bool got_it = ack_cv.wait_for(lock,
            std::chrono::seconds(constants::TIMEOUT_REGISTER_ACK_SEC),
            [&]{ return ack_received; });

        if (!got_it || received_token.empty()) {
            spdlog::error("Onboarding: no valid register_ack received (timeout {}s)",
                          constants::TIMEOUT_REGISTER_ACK_SEC);
            return false;
        }
    }

    // Persist device_id + token (Session 0, Section 2, Étape 5)
    auto& cfg = orbis::ConfigManager::instance();
    cfg.setDeviceId(device_id);
    cfg.setToken(received_token);
    if (!cfg.save()) {
        spdlog::error("Onboarding: failed to save credentials");
        return false;
    }

    spdlog::info("Onboarding complete. device_id={}", device_id);

    // Disconnect so we can reconnect with real credentials
    mqtt.disconnect();
    return true;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // -----------------------------------------------------------------------
    // Parse CLI arguments
    // -----------------------------------------------------------------------
    std::string config_path    = constants::DEFAULT_CONFIG_PATH;
    std::string server_url;
    std::string reg_token;
    std::string uninstall_code;
    bool do_install              = false;
    bool do_uninstall            = false;
    bool do_run                  = false;
    bool do_version              = false;
    bool do_status               = false;
    bool do_verify_uninstall     = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      (arg == "--install")                   { do_install = true; }
        else if (arg == "--uninstall")                 { do_uninstall = true; }
        else if (arg == "--verify-uninstall-code")     { do_verify_uninstall = true; }
        else if (arg == "--run")                       { do_run = true; }
        else if (arg == "--version")                   { do_version = true; }
        else if (arg == "--status")                    { do_status = true; }
        else if (arg == "--config"         && i + 1 < argc) { config_path    = argv[++i]; }
        else if (arg == "--server"         && i + 1 < argc) { server_url     = argv[++i]; }
        else if (arg == "--token"          && i + 1 < argc) { reg_token      = argv[++i]; }
        else if (arg == "--uninstall-code" && i + 1 < argc) { uninstall_code = argv[++i]; }
    }

    // -----------------------------------------------------------------------
    // --version
    // -----------------------------------------------------------------------
    if (do_version) {
        std::cout << constants::AGENT_NAME << " v" << constants::AGENT_VERSION << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // --verify-uninstall-code <code>
    // Used by the NSIS uninstaller and Linux prerm to check the code before
    // proceeding. Exits 0 if correct, 1 if wrong.
    // -----------------------------------------------------------------------
    if (do_verify_uninstall) {
        if (uninstall_code.empty()) {
            std::cerr << "Error: --verify-uninstall-code requires --uninstall-code <code>\n";
            return 1;
        }
        return verifyUninstallCode(uninstall_code) ? 0 : 1;
    }

    // -----------------------------------------------------------------------
    // --install
    // -----------------------------------------------------------------------
    if (do_install) {
        if (server_url.empty()) {
            std::cerr << "Error: --install requires --server <url>\n";
            return 1;
        }

        // Write a minimal config with the server URL before installing
        // (so the service picks up the right broker on first start)
        std::ofstream cfg_out(config_path);
        if (cfg_out) {
            cfg_out << "[agent]\ndevice_id = \"\"\ntoken = \"\"\n"
                    << "log_file = \"" << constants::DEFAULT_LOG_PATH << "\"\n\n"
                    << "[mqtt]\nbroker_host = \"" << server_url << "\"\n"
                    << "broker_port = 8883\nuse_tls = true\n"
                    << "register_secret = \"" << (reg_token.empty()
                        ? "orbis_register_secret" : reg_token) << "\"\n"
                    << "reconnect_delay_min_sec = 1\nreconnect_delay_max_sec = 60\n"
                    << "keepalive_sec = 60\n\n"
                    << "[heartbeat]\ninterval_sec = 30\n\n"
                    << "[logs]\ninterval_sec = 60\n";
        }

        std::string exec_path;
#if defined(PLATFORM_WINDOWS)
        char buf[MAX_PATH]; GetModuleFileNameA(nullptr, buf, MAX_PATH); exec_path = buf;
#elif defined(PLATFORM_LINUX)
        char buf[4096]; ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf)-1);
        if (n > 0) { buf[n] = 0; exec_path = buf; } else exec_path = argv[0];
#elif defined(PLATFORM_MACOS)
        char buf[4096]; uint32_t sz = sizeof(buf);
        _NSGetExecutablePath(buf, &sz); exec_path = buf;
#else
        exec_path = argv[0];
#endif

        // Save the uninstall protection key (default if not provided)
        const std::string code_to_save = uninstall_code.empty()
            ? constants::DEFAULT_UNINSTALL_CODE
            : uninstall_code;
        if (!saveUninstallKey(code_to_save)) {
            std::cerr << "Warning: could not save uninstall key.\n";
        }

        auto installer = orbis::ServiceInstaller::create();
        return installer->install(exec_path, config_path) ? 0 : 1;
    }

    // -----------------------------------------------------------------------
    // --uninstall [--uninstall-code <code>]
    // -----------------------------------------------------------------------
    if (do_uninstall) {
        // Check whether a protection key exists
        std::ifstream key_check(constants::UNINSTALL_KEY_PATH);
        if (key_check.good()) {
            key_check.close();
            if (uninstall_code.empty()) {
                std::cerr << "Error: this agent is protected. "
                             "Provide --uninstall-code <code> to uninstall.\n";
                return 1;
            }
            if (!verifyUninstallCode(uninstall_code)) {
                std::cerr << "Error: invalid uninstall code.\n";
                return 1;
            }
        }

        auto installer = orbis::ServiceInstaller::create();
        bool ok = installer->uninstall();
        if (ok) {
            // Remove the key file after successful uninstall
            std::error_code ec;
            fs::remove(constants::UNINSTALL_KEY_PATH, ec);
        }
        return ok ? 0 : 1;
    }

    // -----------------------------------------------------------------------
    // --status
    // -----------------------------------------------------------------------
    if (do_status) {
        auto installer = orbis::ServiceInstaller::create();
        bool running   = installer->isRunning();
        std::cout << constants::SERVICE_NAME << ": "
                  << (running ? "running" : "stopped") << "\n";
        return running ? 0 : 1;
    }

    // -----------------------------------------------------------------------
    // Normal run (--run or launched by service manager)
    // -----------------------------------------------------------------------

    // 1. Load config
    auto& cfg = orbis::ConfigManager::instance();
    if (!cfg.load(config_path)) {
        std::cerr << "Failed to load config: " << config_path << "\n";
        return 1;
    }

    // 2. Initialise logging
    initLogging(cfg.getLogFilePath(), do_run);
    spdlog::info("=== {} v{} starting ===", constants::AGENT_NAME, constants::AGENT_VERSION);

    // Signal handlers
#if !defined(PLATFORM_WINDOWS)
    std::signal(SIGINT,  handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::signal(SIGQUIT, handleSignal);
    std::signal(SIGHUP,  SIG_IGN);  // config reload not implemented here
#endif

    // 3. LocalBuffer
    auto buffer_db = cfg.getBufferDbPath();
    fs::create_directories(fs::path(buffer_db).parent_path());

    auto mqtt_cfg = cfg.getMQTTConfig();

    // Build broker URI
    std::string broker_uri = (mqtt_cfg.use_tls ? "ssl://" : "tcp://") +
                             mqtt_cfg.broker_host + ":" +
                             std::to_string(mqtt_cfg.broker_port);

    // 4. Create MQTT client
    auto mqtt = std::make_unique<orbis::MQTTClientWrapper>(
        broker_uri,
        "orbis-agent-desktop-" + cfg.getDeviceId(),
        mqtt_cfg.ca_cert_path,
        mqtt_cfg.reconnect_delay_min_sec,
        mqtt_cfg.reconnect_delay_max_sec);

    // LocalBuffer — publish lambda (connected to MQTT)
    auto buffer = std::make_unique<orbis::LocalBuffer>(
        buffer_db,
        [&](const std::string& topic, const std::string& payload) -> bool {
            return mqtt->publish(topic, payload);
        });

    // Flush buffer when MQTT reconnects
    mqtt->setConnectedCallback([&]() {
        spdlog::info("MQTT connected — flushing local buffer");
        std::thread([&]() { buffer->flush(); }).detach();
    });

    // 5. Onboarding if needed
    std::string device_id = cfg.getDeviceId();
    if (!cfg.isRegistered()) {
        spdlog::info("Device not registered — starting onboarding");

        int retry = 0;
        while (!g_shutdown && !cfg.isRegistered()) {
            if (retry > 0) {
                spdlog::info("Onboarding retry in {}s", constants::TIMEOUT_REGISTER_RETRY_SEC);
                for (int i = 0; i < constants::TIMEOUT_REGISTER_RETRY_SEC && !g_shutdown; ++i)
                    std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            // Recreate MQTT client with register client-id for onboarding
            auto reg_mqtt = std::make_unique<orbis::MQTTClientWrapper>(
                broker_uri,
                "orbis-register-desktop-" + device_id,
                mqtt_cfg.ca_cert_path,
                mqtt_cfg.reconnect_delay_min_sec,
                mqtt_cfg.reconnect_delay_max_sec);

            if (doOnboarding(*reg_mqtt, device_id, mqtt_cfg.register_secret)) break;
            ++retry;
        }

        if (g_shutdown) return 0;
        if (!cfg.isRegistered()) {
            spdlog::critical("Onboarding failed — giving up");
            return 1;
        }
        device_id = cfg.getDeviceId();
    }

    // 6. Load plugins BEFORE connecting MQTT.
    //    All dlopen() calls must complete before MQTTClient_connect() starts Paho's background
    //    receive thread.  On macOS, the dyld global lock acquired by dlopen can interact with
    //    Paho's thread in unpredictable ways, causing EMQX to close the connection within 1-2s.
    auto plugin_cfg    = cfg.getPluginConfig();
    auto plugin_loader = std::make_unique<orbis::PluginLoader>(plugin_cfg.plugins_dir);
    if (plugin_cfg.enabled) plugin_loader->loadAll();

    // 7. Connect MQTT with real device credentials + Last Will
    std::string will_topic   = constants::topicFor(constants::TOPIC_STATUS_FMT, device_id);
    json will_payload;
    will_payload["device_id"]  = device_id;
    will_payload["timestamp"]  = nowISO8601();
    will_payload["status"]     = "offline";
    will_payload["version"]    = constants::AGENT_VERSION;
    will_payload["uptime_sec"] = 0;

    spdlog::info("Connecting MQTT as device {}", device_id);
    if (!mqtt->connect(device_id,           // username = device_id
                       cfg.getToken(),       // password = token
                       will_topic,
                       will_payload.dump(),
                       mqtt_cfg.keepalive_sec)) {
        spdlog::critical("MQTT connect failed — aborting");
        return 1;
    }

    // 8. Start services
    auto log_cfg       = cfg.getLogConfig();
    auto hb_cfg        = cfg.getHeartbeatConfig();
    auto scanner_cfg   = cfg.getNetworkScannerConfig();

    // Publish lambda (with LocalBuffer fallback)
    auto publish = [&](const std::string& topic, const std::string& payload) -> bool {
        if (mqtt->publish(topic, payload)) return true;
        buffer->store(topic, payload);
        return false;
    };

    auto publish_retained = [&](const std::string& topic,
                                const std::string& payload,
                                int qos, bool retained) -> bool {
        return mqtt->publish(topic, payload, qos, retained);
    };

    // HeartbeatService
    auto heartbeat = std::make_unique<orbis::HeartbeatService>(
        publish_retained, device_id, hb_cfg.interval_sec);

    // LogCollector
    auto store_fn = [&](const std::string& topic, const std::string& payload) {
        buffer->store(topic, payload);
    };
    auto log_publish = [&](const std::string& topic, const std::string& payload) -> bool {
        return publish(topic, payload);
    };
    auto log_collector = std::make_unique<orbis::LogCollector>(
        log_publish, store_fn,
        plugin_cfg.enabled ? plugin_loader.get() : nullptr,
        device_id, log_cfg.interval_sec, log_cfg.levels);

    // NetworkScanner (optional)
    std::unique_ptr<orbis::NetworkScanner> net_scanner;
    if (scanner_cfg.enabled) {
        net_scanner = std::make_unique<orbis::NetworkScanner>(
            [&](const std::string& t, const std::string& p){ return publish(t, p); },
            device_id, scanner_cfg.scan_interval_sec, scanner_cfg.port_range);
    }

    // UpdateManager (shared, used by CommandExecutor)
    auto update_manager = std::make_unique<orbis::UpdateManager>(
        [&](const std::string& t, const std::string& p){ return publish(t, p); },
        device_id);

    // CommandExecutor — wires itself to MQTT callbacks
    auto cmd_executor = std::make_unique<orbis::CommandExecutor>(
        [&](const std::string& t, const std::string& p){ return publish(t, p); },
        [&](const std::string& t){ return mqtt->subscribe(t); },
        [&](std::function<void(const std::string&, const std::string&)> cb) {
            mqtt->setMessageCallback(std::move(cb));
        },
        device_id,
        log_collector.get(),
        net_scanner.get(),
        update_manager.get());

    // Start all services
    heartbeat->start();
    log_collector->start();
    cmd_executor->start();
    if (net_scanner) net_scanner->start();

    spdlog::info("All services started. Device {} is online.", device_id);

    // Attach MQTT log sink — forward INFO+ logs to backend
    {
        auto agent_log_topic = std::string("devices/") + device_id + "/agent_logs";
        auto mqtt_sink = std::make_shared<MqttLogSink>(publish_retained, device_id, agent_log_topic);
        mqtt_sink->set_level(spdlog::level::info);
        spdlog::default_logger()->sinks().push_back(mqtt_sink);
        spdlog::info("MQTT log sink attached — agent logs forwarded to backend");
    }

    // 9. Watchdog loop — restart dead services every INTERVAL_WATCHDOG_SEC seconds
    while (!g_shutdown) {
        std::unique_lock<std::mutex> lock(g_shutdown_mutex);
        g_shutdown_cv.wait_for(lock, std::chrono::seconds(constants::INTERVAL_WATCHDOG_SEC),
                               []{ return g_shutdown.load(); });
        if (g_shutdown) break;

        if (!heartbeat->isRunning()) {
            spdlog::warn("Watchdog: restarting HeartbeatService");
            heartbeat->start();
        }
        if (!log_collector->isRunning()) {
            spdlog::warn("Watchdog: restarting LogCollector");
            log_collector->start();
        }
        if (net_scanner && !net_scanner->isRunning()) {
            spdlog::warn("Watchdog: restarting NetworkScanner");
            net_scanner->start();
        }
    }

    // 10. Graceful shutdown
    spdlog::info("Shutdown signal received — stopping services");

    cmd_executor->stop();
    if (net_scanner) net_scanner->stop();
    log_collector->stop();   // also publishes final log batch
    heartbeat->stop();       // publishes "offline" status

    if (plugin_cfg.enabled) plugin_loader->unloadAll();

    // Flush any remaining buffered messages
    if (mqtt->isConnected()) {
        buffer->flush();
    }

    mqtt->disconnect();
    spdlog::info("=== {} stopped ===", constants::AGENT_NAME);
    return 0;
}
