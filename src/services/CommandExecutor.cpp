#include "CommandExecutor.hpp"
#include "LogCollector.hpp"
#include "NetworkScanner.hpp"
#include "UpdateManager.hpp"
#include "constants.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <array>
#include <chrono>
#include <ctime>
#include <future>
#include <sstream>
#include <thread>

#if defined(PLATFORM_WINDOWS)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <sys/wait.h>
#  include <unistd.h>
#endif

using json = nlohmann::json;

namespace orbis {

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

CommandExecutor::CommandExecutor(
    std::function<bool(const std::string&, const std::string&)> publish_fn,
    std::function<bool(const std::string&)>                     subscribe_fn,
    std::function<void(std::function<void(const std::string&,
                                          const std::string&)>)> set_msg_cb,
    const std::string& device_id,
    LogCollector*      log_collector,
    NetworkScanner*    network_scanner,
    UpdateManager*     update_manager)
    : publish_fn_(std::move(publish_fn))
    , subscribe_fn_(std::move(subscribe_fn))
    , device_id_(device_id)
    , log_collector_(log_collector)
    , network_scanner_(network_scanner)
    , update_manager_(update_manager)
{
    // Wire up the inbound message callback on the MQTT client
    set_msg_cb([this](const std::string& topic, const std::string& payload) {
        std::string commands_topic = constants::topicFor(constants::TOPIC_COMMANDS_FMT, device_id_);
        std::string update_topic   = constants::topicFor(constants::TOPIC_UPDATE_FMT,   device_id_);
        if (topic == commands_topic || topic == update_topic) {
            processCommand(topic, payload);
        }
    });
}

CommandExecutor::~CommandExecutor() { stop(); }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void CommandExecutor::start() {
    if (running_) return;
    stop_requested_ = false;
    running_        = true;

    // Subscribe to command and update topics
    subscribe_fn_(constants::topicFor(constants::TOPIC_COMMANDS_FMT, device_id_));
    subscribe_fn_(constants::topicFor(constants::TOPIC_UPDATE_FMT,   device_id_));

    // Start worker thread that drains the command queue
    worker_thread_ = std::thread(&CommandExecutor::workerLoop, this);

    spdlog::info("CommandExecutor ready, subscribed to commands + update");
}

void CommandExecutor::stop() {
    stop_requested_ = true;
    running_ = false;
    queue_cv_.notify_all();
    if (worker_thread_.joinable()) worker_thread_.join();
}

bool CommandExecutor::isRunning() const { return running_; }

// ---------------------------------------------------------------------------
// Process incoming command (called from MQTT callback thread — must not block)
// ---------------------------------------------------------------------------

void CommandExecutor::processCommand(const std::string& /*topic*/,
                                     const std::string& payload)
{
    // Just enqueue and return immediately so the Paho callback is not blocked.
    // The worker thread will execute and publish results outside this context.
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        command_queue_.push(payload);
    }
    queue_cv_.notify_one();
}

// ---------------------------------------------------------------------------
// Worker thread — drains queue and executes commands (publish-safe context)
// ---------------------------------------------------------------------------

void CommandExecutor::workerLoop() {
    while (!stop_requested_) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this] {
            return stop_requested_ || !command_queue_.empty();
        });

        while (!command_queue_.empty()) {
            std::string payload = std::move(command_queue_.front());
            command_queue_.pop();
            lock.unlock();
            executeCommand(payload);
            lock.lock();
        }
    }
}

// ---------------------------------------------------------------------------
// Execute a single command (called from worker thread)
// ---------------------------------------------------------------------------

void CommandExecutor::executeCommand(const std::string& payload) {
    std::string command_id;
    try {
        auto j = json::parse(payload);
        command_id = j.value("command_id", "");
        std::string type = j.value("type", "");
        int timeout_sec  = j.value("timeout_sec", constants::TIMEOUT_COMMAND_DEFAULT_SEC);

        spdlog::info("Command received: {} (type={})", command_id, type);

        // Step 1: acknowledged
        publishResult(command_id, "acknowledged", "", "", -1);

        if      (type == "shell")           handleShell(command_id, j.value("payload", json{}).dump(), timeout_sec);
        else if (type == "restart_service") handleRestart(command_id);
        else if (type == "collect_now")     handleCollectNow(command_id);
        else if (type == "scan_network")    handleScanNetwork(command_id);
        else if (type == "get_info")        handleGetInfo(command_id);
        else if (type == "agent_update") {
            if (!update_manager_) {
                spdlog::error("agent_update: UpdateManager not available");
                publishResult(command_id, "failed", "", "UpdateManager not configured", -1);
            } else {
                auto payload_j = j.value("payload", json{});
                std::string url     = payload_j.value("url",     "");
                std::string sha256  = payload_j.value("sha256",  "");
                std::string version = payload_j.value("version", "");

                if (url.empty() || sha256.empty()) {
                    publishResult(command_id, "failed", "", "Missing url or sha256 in payload", -1);
                } else {
                    // Run update in a detached thread — it is blocking (download + verify + replace + restart)
                    auto* um  = update_manager_;
                    auto  cmd = command_id;
                    std::thread([um, cmd, url, sha256, version]() {
                        um->update(cmd, url, sha256, version);
                    }).detach();
                }
            }
        } else {
            spdlog::warn("Unknown command type: {}", type);
            publishResult(command_id, "failed", "", "Unknown command type: " + type, -1);
        }

    } catch (const std::exception& e) {
        spdlog::error("CommandExecutor: exception processing command: {}", e.what());
        if (!command_id.empty()) {
            publishResult(command_id, "failed", "", e.what(), -1);
        }
    }
}

// ---------------------------------------------------------------------------
// publishResult — Section 6 format (Session 0 has priority)
// ---------------------------------------------------------------------------

void CommandExecutor::publishResult(const std::string& command_id,
                                    const std::string& statut,
                                    const std::string& output,
                                    const std::string& error,
                                    int exit_code)
{
    json j;
    j["command_id"] = command_id;
    j["device_id"]  = device_id_;
    j["statut"]     = statut;
    j["timestamp"]  = nowISO8601();

    if (statut == "success" || statut == "failed") {
        j["output"]    = output.empty() ? json(nullptr) : json(output);
        j["error"]     = error.empty()  ? json(nullptr) : json(error);
        j["exit_code"] = (exit_code < 0) ? json(nullptr) : json(exit_code);

        if (statut == "success")
            spdlog::info("Command success: {} exit_code={}", command_id, exit_code);
        else
            spdlog::warn("Command failed:  {} exit_code={} error={}", command_id, exit_code, error);
    } else {
        j["output"]    = nullptr;
        j["error"]     = nullptr;
        j["exit_code"] = nullptr;

        spdlog::debug("Command {}: statut={}", command_id, statut);
    }

    std::string topic = constants::topicFor(constants::TOPIC_RESULTS_FMT, device_id_);
    if (!publish_fn_(topic, j.dump())) {
        spdlog::warn("CommandExecutor: failed to publish result for {} (statut={})", command_id, statut);
    }
}

// ---------------------------------------------------------------------------
// Shell command handler
// ---------------------------------------------------------------------------

void CommandExecutor::handleShell(const std::string& cmd_id,
                                  const std::string& payload_str,
                                  int timeout_sec)
{
    std::string command;
    try {
        auto payload = json::parse(payload_str);
        command = payload.value("command", "");
    } catch (...) {
        command = payload_str; // treat raw string as command if not JSON
    }
    if (command.empty()) {
        publishResult(cmd_id, "failed", "", "Empty command", -1);
        return;
    }

    publishResult(cmd_id, "executing", "", "", -1);

    auto result = runShell(command, timeout_sec);

    // Truncate to 64 KB
    if (result.output.size() > constants::COMMAND_OUTPUT_MAX_BYTES) {
        result.output.resize(constants::COMMAND_OUTPUT_MAX_BYTES);
        result.output += "\n[TRUNCATED]";
    }

    std::string statut = (result.exit_code == 0) ? "success" : "failed";
    publishResult(cmd_id, statut, result.output, result.error, result.exit_code);
}

// ---------------------------------------------------------------------------
// Platform shell execution
// ---------------------------------------------------------------------------
#if defined(PLATFORM_WINDOWS)

CommandExecutor::ShellResult CommandExecutor::runShell(const std::string& command,
                                                        int timeout_sec)
{
    ShellResult res{-1, "", ""};
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE hStdOutRd, hStdOutWr;
    CreatePipe(&hStdOutRd, &hStdOutWr, &sa, 0);
    SetHandleInformation(hStdOutRd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb         = sizeof(si);
    si.hStdOutput = hStdOutWr;
    si.hStdError  = hStdOutWr;
    si.dwFlags    = STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi{};
    std::string cmd = "cmd.exe /c " + command;
    if (!CreateProcessA(nullptr, const_cast<char*>(cmd.c_str()),
                        nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hStdOutRd); CloseHandle(hStdOutWr);
        res.error = "CreateProcess failed";
        return res;
    }
    CloseHandle(hStdOutWr);

    DWORD wait_ms = static_cast<DWORD>(timeout_sec) * 1000;
    DWORD rc = WaitForSingleObject(pi.hProcess, wait_ms);

    if (rc == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        res.error = "Command timed out";
    } else {
        DWORD exit_code = 0;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        res.exit_code = static_cast<int>(exit_code);
    }

    // Read stdout/stderr
    char buf[4096];
    DWORD read = 0;
    while (ReadFile(hStdOutRd, buf, sizeof(buf) - 1, &read, nullptr) && read > 0) {
        buf[read] = 0;
        res.output += buf;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hStdOutRd);
    return res;
}

#else

CommandExecutor::ShellResult CommandExecutor::runShell(const std::string& command,
                                                        int timeout_sec)
{
    ShellResult res{-1, "", ""};

    // Use popen with a future + alarm-style timeout
    auto fut = std::async(std::launch::async, [&]() -> ShellResult {
        ShellResult r{-1, "", ""};
        FILE* fp = popen(command.c_str(), "r");
        if (!fp) { r.error = "popen failed"; return r; }
        char buf[4096];
        while (fgets(buf, sizeof(buf), fp)) r.output += buf;
        int status = pclose(fp);
        r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return r;
    });

    if (fut.wait_for(std::chrono::seconds(timeout_sec)) == std::future_status::timeout) {
        res.error     = "Command timed out after " + std::to_string(timeout_sec) + "s";
        res.exit_code = -1;
        return res;
    }
    return fut.get();
}
#endif

// ---------------------------------------------------------------------------
// Other command handlers
// ---------------------------------------------------------------------------

void CommandExecutor::handleRestart(const std::string& cmd_id) {
    publishResult(cmd_id, "executing", "", "", -1);

#if defined(PLATFORM_LINUX)
    auto result = runShell("systemctl restart orbis-agent", 30);
#elif defined(PLATFORM_MACOS)
    auto result = runShell("launchctl unload " + std::string(constants::LAUNCHD_PLIST_PATH) +
                           " && launchctl load " + constants::LAUNCHD_PLIST_PATH, 30);
#elif defined(PLATFORM_WINDOWS)
    auto result = runShell("net stop orbis-agent & net start orbis-agent", 60);
#endif
    std::string statut = (result.exit_code == 0) ? "success" : "failed";
    publishResult(cmd_id, statut, result.output, result.error, result.exit_code);
}

void CommandExecutor::handleCollectNow(const std::string& cmd_id) {
    publishResult(cmd_id, "executing", "", "", -1);
    if (log_collector_) log_collector_->triggerNow();
    publishResult(cmd_id, "success", "Collection triggered", "", 0);
}

void CommandExecutor::handleScanNetwork(const std::string& cmd_id) {
    publishResult(cmd_id, "executing", "", "", -1);
    if (network_scanner_) network_scanner_->triggerNow();
    publishResult(cmd_id, "success", "Network scan triggered", "", 0);
}

void CommandExecutor::handleGetInfo(const std::string& cmd_id) {
    publishResult(cmd_id, "executing", "", "", -1);
    // Return agent version and device_id as info
    json info;
    info["device_id"] = device_id_;
    info["version"]   = constants::AGENT_VERSION;
    publishResult(cmd_id, "success", info.dump(), "", 0);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string CommandExecutor::nowISO8601() {
    auto now   = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    char buf[32] = {};
    struct tm tm_buf{};
#if defined(PLATFORM_WINDOWS)
    gmtime_s(&tm_buf, &now_t);
#else
    gmtime_r(&now_t, &tm_buf);
#endif
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

} // namespace orbis
