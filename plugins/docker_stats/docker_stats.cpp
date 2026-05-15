/**
 * @file docker_stats.cpp
 * @brief Orbis plugin — Docker container CPU/RAM stats via Unix socket.
 *
 * Connects to /var/run/docker.sock and queries:
 *   GET /containers/json          → list of running containers
 *   GET /containers/{id}/stats?stream=false → one-shot stats
 *
 * Uses libcurl with CURLOPT_UNIX_SOCKET_PATH.
 * On Windows, Docker Desktop exposes a named pipe — not supported here.
 */
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// HTTP GET via Docker Unix socket
// ---------------------------------------------------------------------------

static size_t curlWrite(void* ptr, size_t sz, size_t nmemb, void* ud) {
    static_cast<std::string*>(ud)->append(static_cast<char*>(ptr), sz * nmemb);
    return sz * nmemb;
}

static std::string dockerGet(const std::string& path) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";
    struct Guard { CURL* c; ~Guard() { curl_easy_cleanup(c); } } g{curl};

    std::string resp;
    std::string url = "http://localhost" + path;

    curl_easy_setopt(curl, CURLOPT_URL,               url.c_str());
    curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH,  "/var/run/docker.sock");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,     curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,         &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,           5L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) return "";
    return resp;
}

// ---------------------------------------------------------------------------
// Compute CPU % from Docker stats JSON
// ---------------------------------------------------------------------------

static double calcCpuPercent(const json& stats) {
    try {
        auto& cpu_s  = stats["cpu_stats"];
        auto& precpu = stats["precpu_stats"];
        double cpu_delta = static_cast<double>(
            cpu_s["cpu_usage"]["total_usage"].get<uint64_t>() -
            precpu["cpu_usage"]["total_usage"].get<uint64_t>());
        double sys_delta = static_cast<double>(
            cpu_s["system_cpu_usage"].get<uint64_t>() -
            precpu["system_cpu_usage"].get<uint64_t>());
        int num_cpus = static_cast<int>(cpu_s["online_cpus"].get<int>());
        if (sys_delta == 0.0) return 0.0;
        return (cpu_delta / sys_delta) * num_cpus * 100.0;
    } catch (...) {
        return 0.0;
    }
}

// ---------------------------------------------------------------------------
// Plugin ABI
// ---------------------------------------------------------------------------

extern "C" {

const char* orbis_plugin_name()    { return "docker_stats"; }
const char* orbis_plugin_version() { return "1.0.0"; }

const char* collect() {
    static std::string result;
    result = "[]";

#if !defined(PLATFORM_WINDOWS)
    try {
        // 1. List running containers
        std::string containers_raw = dockerGet("/containers/json");
        if (containers_raw.empty()) return result.c_str();

        auto containers = json::parse(containers_raw, nullptr, false);
        if (!containers.is_array()) return result.c_str();

        json arr = json::array();

        for (auto& c : containers) {
            std::string id   = c.value("Id", "").substr(0, 12);
            std::string name = !c["Names"].empty()
                               ? c["Names"][0].get<std::string>().substr(1) // strip leading /
                               : id;
            std::string image = c.value("Image", "unknown");

            // 2. Fetch stats (one-shot)
            std::string stats_raw = dockerGet("/containers/" + id + "/stats?stream=false");
            if (stats_raw.empty()) continue;

            auto stats = json::parse(stats_raw, nullptr, false);
            if (stats.is_discarded()) continue;

            double cpu_pct = calcCpuPercent(stats);
            double ram_mb  = 0.0, ram_limit_mb = 0.0, ram_pct = 0.0;
            try {
                auto& mem = stats["memory_stats"];
                double usage = static_cast<double>(mem["usage"].get<uint64_t>());
                double limit = static_cast<double>(mem["limit"].get<uint64_t>());
                ram_mb       = usage / (1024.0 * 1024.0);
                ram_limit_mb = limit / (1024.0 * 1024.0);
                ram_pct      = (limit > 0) ? (usage / limit) * 100.0 : 0.0;
            } catch (...) {}

            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Container %s (%s): CPU %.1f%% RAM %.1f MB",
                     name.c_str(), image.c_str(), cpu_pct, ram_mb);

            json entry;
            entry["source"]  = "docker_plugin";
            entry["level"]   = "INFO";
            entry["message"] = msg;
            entry["metadata"] = {
                {"container_id",   id},
                {"container_name", name},
                {"image",          image},
                {"cpu_percent",    cpu_pct},
                {"ram_mb",         ram_mb},
                {"ram_limit_mb",   ram_limit_mb},
                {"ram_percent",    ram_pct}
            };
            arr.push_back(entry);
        }
        result = arr.dump();
    } catch (...) {
        result = "[]";
    }
#endif
    return result.c_str();
}

void cleanup() {}

} // extern "C"
