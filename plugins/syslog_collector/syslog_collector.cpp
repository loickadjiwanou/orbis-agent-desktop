/**
 * @file syslog_collector.cpp
 * @brief Orbis plugin — system log collection.
 *
 * Linux  : journalctl -n 50 --output=json --no-pager
 * macOS  : log show --last 1m --style json
 * Windows: ReadEventLog (Application + System, last 50 entries each)
 */
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#if defined(PLATFORM_WINDOWS)
#  include <windows.h>
#endif

using json = nlohmann::json;

static std::string mapLevel(int priority) {
    if (priority <= 2) return "CRITICAL";
    if (priority == 3) return "ERROR";
    if (priority == 4) return "WARNING";
    if (priority <= 6) return "INFO";
    return "DEBUG";
}

// ---------------------------------------------------------------------------
// Platform implementations
// ---------------------------------------------------------------------------

#if defined(PLATFORM_LINUX)
static std::string doCollect() {
    json arr = json::array();

    FILE* fp = popen("journalctl -n 50 --output=json --no-pager 2>/dev/null", "r");
    if (!fp) return arr.dump();
    struct PipeGuard { FILE* p; ~PipeGuard() { pclose(p); } } pg{fp};

    char line[8192];
    while (fgets(line, sizeof(line), fp)) {
        try {
            auto j = json::parse(line, nullptr, false);
            if (j.is_discarded()) continue;

            std::string msg   = j.value("MESSAGE", "");
            std::string ident = j.value("SYSLOG_IDENTIFIER", "system");
            std::string prio  = j.value("PRIORITY", "6");
            std::string level = mapLevel(std::stoi(prio));

            if (msg.empty()) continue;
            json entry;
            entry["source"]  = "syslog_plugin";
            entry["level"]   = level;
            entry["message"] = msg;
            entry["metadata"] = {{"ident", ident}};
            arr.push_back(entry);
        } catch (...) {}
    }
    return arr.dump();
}

#elif defined(PLATFORM_MACOS)
static std::string doCollect() {
    json arr = json::array();

    // Use --last 30s to keep the payload small (--last 1m can produce tens of
    // thousands of entries on an active Mac, resulting in MB-sized JSON that
    // may exceed EMQX's max_packet_size and cause the broker to close the
    // connection).  Cap at 50 entries to bound memory and publish latency.
    FILE* fp = popen("log show --last 30s --style json 2>/dev/null", "r");
    if (!fp) return arr.dump();
    struct PipeGuard { FILE* p; ~PipeGuard() { pclose(p); } } pg{fp};

    std::ostringstream buf;
    char chunk[8192];
    while (fgets(chunk, sizeof(chunk), fp)) buf << chunk;

    try {
        auto root = json::parse(buf.str(), nullptr, false);
        if (!root.is_array()) return arr.dump();
        int count = 0;
        for (auto& item : root) {
            if (count >= 50) break;
            std::string msg  = item.value("eventMessage", "");
            std::string cat  = item.value("category", "system");
            std::string type = item.value("messageType", "Default");
            std::string level = (type == "Error")  ? "ERROR"
                              : (type == "Fault")  ? "CRITICAL"
                              : (type == "Debug")  ? "DEBUG"
                              : "INFO";
            if (msg.empty()) continue;
            json entry;
            entry["source"]  = "syslog_plugin";
            entry["level"]   = level;
            entry["message"] = msg;
            entry["metadata"] = {{"category", cat}};
            arr.push_back(entry);
            ++count;
        }
    } catch (...) {}

    return arr.dump();
}

#elif defined(PLATFORM_WINDOWS)
static std::string doCollect() {
    json arr = json::array();

    for (auto* logname : {"System", "Application"}) {
        HANDLE hLog = OpenEventLogA(nullptr, logname);
        if (!hLog) continue;
        struct LogGuard { HANDLE h; ~LogGuard() { CloseEventLog(h); } } lg{hLog};

        std::vector<BYTE> buf(65536);
        DWORD read = 0, needed = 0;
        int count = 0;

        while (count < 50 &&
               ReadEventLogA(hLog,
                             EVENTLOG_SEQUENTIAL_READ | EVENTLOG_BACKWARDS_READ,
                             0, buf.data(),
                             static_cast<DWORD>(buf.size()), &read, &needed))
        {
            BYTE* ptr = buf.data();
            BYTE* end = buf.data() + read;
            while (ptr < end && count < 50) {
                auto* rec = reinterpret_cast<EVENTLOGRECORD*>(ptr);
                std::string level;
                switch (rec->EventType) {
                    case EVENTLOG_ERROR_TYPE:   level = "ERROR";   break;
                    case EVENTLOG_WARNING_TYPE: level = "WARNING"; break;
                    default:                    level = "INFO";    break;
                }
                char* src = reinterpret_cast<char*>(rec) + sizeof(EVENTLOGRECORD);
                std::string msg = src;
                if (!msg.empty()) {
                    json entry;
                    entry["source"]  = "syslog_plugin";
                    entry["level"]   = level;
                    entry["message"] = msg;
                    entry["metadata"] = {
                        {"log",      logname},
                        {"event_id", static_cast<int>(rec->EventID & 0xFFFF)}
                    };
                    arr.push_back(entry);
                    ++count;
                }
                ptr += rec->Length;
            }
        }
    }
    return arr.dump();
}
#else
static std::string doCollect() { return "[]"; }
#endif

// ---------------------------------------------------------------------------
// Plugin ABI
// ---------------------------------------------------------------------------

extern "C" {

const char* orbis_plugin_name()    { return "syslog_collector"; }
const char* orbis_plugin_version() { return "1.0.0"; }

const char* collect() {
    static std::string result;
    try {
        result = doCollect();
    } catch (...) {
        result = "[]";
    }
    return result.c_str();
}

void cleanup() {}

} // extern "C"
