#include "LogCollector.hpp"
#include "plugins/PluginLoader.hpp"
#include "constants.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>

#if defined(PLATFORM_WINDOWS)
#  include <windows.h>
#  include <winevt.h>
#  pragma comment(lib, "wevtapi.lib")
#endif

using json = nlohmann::json;

namespace orbis {

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

LogCollector::LogCollector(
    std::function<bool(const std::string&, const std::string&)>  publish_fn,
    std::function<void(const std::string&, const std::string&)>  store_fn,
    PluginLoader* plugin_loader,
    const std::string& device_id,
    int interval_sec,
    std::vector<std::string> log_levels)
    : publish_fn_(std::move(publish_fn))
    , store_fn_(std::move(store_fn))
    , plugin_loader_(plugin_loader)
    , device_id_(device_id)
    , interval_sec_(interval_sec)
    , log_levels_(std::move(log_levels))
{}

LogCollector::~LogCollector() { stop(); }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void LogCollector::start() {
    if (running_) return;
    stop_requested_ = false;
    running_        = true;
    thread_         = std::thread(&LogCollector::run, this);
}

void LogCollector::stop() {
    stop_requested_ = true;
    trigger_now_    = true; // wake the sleep loop
    if (thread_.joinable()) thread_.join();
    running_ = false;
}

bool LogCollector::isRunning() const { return running_; }

void LogCollector::triggerNow() { trigger_now_ = true; }

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void LogCollector::run() {
#if defined(PLATFORM_LINUX)
    pthread_setname_np(pthread_self(), "log-collector");
#endif
    spdlog::info("LogCollector started (interval={}s)", interval_sec_);

    while (!stop_requested_) {
        try {
            collectAndPublish();
        } catch (const std::exception& e) {
            spdlog::error("LogCollector: {}", e.what());
        }

        // Sleep in 1s ticks, wake early if triggerNow() called
        for (int i = 0; i < interval_sec_ && !stop_requested_ && !trigger_now_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        trigger_now_ = false;
    }
    spdlog::info("LogCollector stopped");
}

// ---------------------------------------------------------------------------
// Collect and publish
// ---------------------------------------------------------------------------

void LogCollector::collectAndPublish() {
    std::vector<LogEntry> entries;

    // From plugins (cpu_ram, disk, syslog_collector, docker_stats, …)
    auto plugin_entries = collectFromPlugins();
    entries.insert(entries.end(), plugin_entries.begin(), plugin_entries.end());

    // From OS native sources — only when no plugin_loader is active.
    // When plugins are enabled, the syslog_collector plugin already runs
    // "log show / journalctl" and provides the same data.  Running both
    // would call "log show --last Xs" twice per cycle, doubling the CPU cost
    // and potentially producing MB-sized payloads that exceed EMQX packet limits.
    if (!plugin_loader_) {
        auto sys_entries = collectFromSystem();
        entries.insert(entries.end(), sys_entries.begin(), sys_entries.end());
    }

    if (entries.empty()) return;

    // Batch into groups of BATCH_MAX and publish
    std::string topic = constants::topicFor(constants::TOPIC_LOGS_FMT, device_id_);

    for (size_t start = 0; start < entries.size(); start += constants::LOG_BATCH_MAX) {
        size_t end  = std::min(start + static_cast<size_t>(constants::LOG_BATCH_MAX),
                               entries.size());
        json batch  = json::array();

        for (size_t i = start; i < end; ++i) {
            auto& e = entries[i];
            json item;
            item["device_id"] = e.device_id;
            item["timestamp"] = e.timestamp;
            item["level"]     = e.level;
            item["source"]    = e.source;
            item["message"]   = e.message;
            item["metadata"]  = json::parse(e.metadata_json,
                                            nullptr, /*ex=*/false);
            if (item["metadata"].is_discarded()) item["metadata"] = json::object();
            batch.push_back(std::move(item));
        }

        std::string payload = batch.dump();
        if (!publish_fn_(topic, payload)) {
            spdlog::warn("LogCollector: publish failed, buffering {} entries", batch.size());
            store_fn_(topic, payload);
        }
    }
}

// ---------------------------------------------------------------------------
// Collect from plugins
// ---------------------------------------------------------------------------

std::vector<LogCollector::LogEntry> LogCollector::collectFromPlugins() {
    std::vector<LogEntry> result;
    if (!plugin_loader_) return result;

    plugin_loader_->collectAll([&](std::vector<PluginLogEntry> plugin_entries) {
        for (auto& pe : plugin_entries) {
            if (!levelAllowed(pe.level)) continue;
            LogEntry e;
            e.device_id     = device_id_;
            e.timestamp     = nowISO8601();
            e.level         = pe.level;
            e.source        = pe.source;
            e.message       = pe.message;
            e.metadata_json = pe.metadata_json;
            result.push_back(std::move(e));
        }
    });
    return result;
}

// ---------------------------------------------------------------------------
// Collect from OS native log source
// ---------------------------------------------------------------------------

#if defined(PLATFORM_LINUX)
std::vector<LogCollector::LogEntry> LogCollector::collectFromSystem() {
    std::vector<LogEntry> result;

    // journalctl -n 50 --output=json --no-pager
    FILE* fp = popen("journalctl -n 50 --output=json --no-pager 2>/dev/null", "r");
    if (!fp) return result;

    struct PipeGuard { FILE* p; ~PipeGuard() { pclose(p); } } pg{fp};

    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        try {
            auto j = json::parse(line, nullptr, false);
            if (j.is_discarded()) continue;

            std::string msg     = j.value("MESSAGE", "");
            std::string ident   = j.value("SYSLOG_IDENTIFIER", "syslog_plugin");
            std::string prio_s  = j.value("PRIORITY", "6");
            int prio = std::stoi(prio_s);

            // Map syslog priority → level
            std::string level;
            if (prio <= 2)      level = "CRITICAL";
            else if (prio == 3) level = "ERROR";
            else if (prio == 4) level = "WARNING";
            else if (prio <= 6) level = "INFO";
            else                level = "DEBUG";

            if (!levelAllowed(level) || msg.empty()) continue;

            LogEntry e;
            e.device_id     = device_id_;
            e.timestamp     = nowISO8601();
            e.level         = level;
            e.source        = "syslog_plugin";
            e.message       = msg;
            e.metadata_json = R"({"ident":")" + ident + R"("})";
            result.push_back(std::move(e));
        } catch (...) {}
    }
    return result;
}

#elif defined(PLATFORM_MACOS)
std::vector<LogCollector::LogEntry> LogCollector::collectFromSystem() {
    std::vector<LogEntry> result;

    FILE* fp = popen("log show --last 1m --style json 2>/dev/null", "r");
    if (!fp) return result;
    struct PipeGuard { FILE* p; ~PipeGuard() { pclose(p); } } pg{fp};

    std::ostringstream buf;
    char chunk[4096];
    while (fgets(chunk, sizeof(chunk), fp)) buf << chunk;

    try {
        auto arr = json::parse(buf.str(), nullptr, false);
        if (!arr.is_array()) return result;
        for (auto& item : arr) {
            std::string msg   = item.value("eventMessage", "");
            std::string cat   = item.value("category", "syslog_plugin");
            std::string level = "INFO";
            std::string t     = item.value("messageType", "Default");
            if (t == "Error")  level = "ERROR";
            if (t == "Fault")  level = "CRITICAL";
            if (t == "Debug")  level = "DEBUG";

            if (!levelAllowed(level) || msg.empty()) continue;
            LogEntry e;
            e.device_id     = device_id_;
            e.timestamp     = nowISO8601();
            e.level         = level;
            e.source        = "syslog_plugin";
            e.message       = msg;
            e.metadata_json = R"({"category":")" + cat + R"("})";
            result.push_back(std::move(e));
        }
    } catch (...) {}
    return result;
}

#elif defined(PLATFORM_WINDOWS)
std::vector<LogCollector::LogEntry> LogCollector::collectFromSystem() {
    std::vector<LogEntry> result;
    // Windows Event Log — read last 50 entries from System log
    HANDLE hLog = OpenEventLogA(nullptr, "System");
    if (!hLog) return result;
    struct LogGuard { HANDLE h; ~LogGuard() { CloseEventLog(h); } } lg{hLog};

    std::vector<BYTE> buf(65536);
    DWORD read = 0, needed = 0;
    while (ReadEventLogA(hLog, EVENTLOG_SEQUENTIAL_READ | EVENTLOG_BACKWARDS_READ,
                         0, buf.data(), static_cast<DWORD>(buf.size()), &read, &needed)) {
        BYTE* ptr = buf.data();
        BYTE* end = buf.data() + read;
        while (ptr < end) {
            auto* rec = reinterpret_cast<EVENTLOGRECORD*>(ptr);
            std::string level;
            switch (rec->EventType) {
                case EVENTLOG_ERROR_TYPE:   level = "ERROR";   break;
                case EVENTLOG_WARNING_TYPE: level = "WARNING"; break;
                default:                   level = "INFO";     break;
            }
            if (levelAllowed(level)) {
                char* src = reinterpret_cast<char*>(rec) + sizeof(EVENTLOGRECORD);
                LogEntry e;
                e.device_id     = device_id_;
                e.timestamp     = nowISO8601();
                e.level         = level;
                e.source        = "syslog_plugin";
                e.message       = src;
                e.metadata_json = R"({"event_id":)" + std::to_string(rec->EventID) + "}";
                result.push_back(std::move(e));
                if (result.size() >= 50) return result;
            }
            ptr += rec->Length;
        }
    }
    return result;
}
#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool LogCollector::levelAllowed(const std::string& level) const {
    if (log_levels_.empty()) return true;
    return std::find(log_levels_.begin(), log_levels_.end(), level) != log_levels_.end();
}

std::string LogCollector::nowISO8601() {
    auto now  = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now_t));
    return buf;
}

} // namespace orbis
