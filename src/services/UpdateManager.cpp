#include "UpdateManager.hpp"
#include "constants.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <curl/curl.h>
#include <openssl/evp.h>

#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#if defined(PLATFORM_WINDOWS)
#  include <windows.h>
#else
#  include <unistd.h>
#endif
#if defined(PLATFORM_MACOS)
#  include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace orbis {

// ---------------------------------------------------------------------------
// libcurl write callback
// ---------------------------------------------------------------------------
static size_t curlWrite(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* stream = static_cast<std::ofstream*>(userdata);
    stream->write(static_cast<const char*>(ptr), static_cast<std::streamsize>(size * nmemb));
    return size * nmemb;
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

UpdateManager::UpdateManager(
    std::function<bool(const std::string&, const std::string&)> publish_fn,
    const std::string& device_id)
    : publish_fn_(std::move(publish_fn)), device_id_(device_id)
{}

// ---------------------------------------------------------------------------
// Main update workflow
// ---------------------------------------------------------------------------

void UpdateManager::update(const std::string& command_id,
                           const std::string& url,
                           const std::string& sha256,
                           const std::string& version)
{
    spdlog::info("UpdateManager: starting update to {} ({})", version, url);

    std::string binary_path = currentBinaryPath();
    std::string backup_path = binary_path + ".bak";
    std::string tmp_path    = binary_path + ".new";

    // Step 1: acknowledged already published by CommandExecutor

    // Step 2: backup current binary (silent, no progress step)
    try {
        fs::copy_file(binary_path, backup_path, fs::copy_options::overwrite_existing);
        spdlog::info("UpdateManager: backup created at {}", backup_path);
    } catch (const std::exception& e) {
        publishResult(command_id, "failed", "", "Backup failed: " + std::string(e.what()));
        return;
    }

    // Step 3: download
    publishProgress(command_id, "downloading", "Downloading " + url);
    if (!downloadBinary(url, tmp_path)) {
        fs::remove(tmp_path);
        publishProgress(command_id, "failed", "", "Download failed: " + url);
        publishResult(command_id, "failed", "", "Download failed: " + url);
        return;
    }

    // Step 4: verify SHA-256
    publishProgress(command_id, "verifying", "Verifying SHA-256");
    if (!sha256.empty() && !verifySHA256(tmp_path, sha256)) {
        fs::remove(tmp_path);
        publishProgress(command_id, "failed", "", "SHA256 mismatch");
        publishResult(command_id, "failed", "", "SHA256 mismatch");
        return;
    }

    // Step 5: replace binary
    publishProgress(command_id, "replacing", "Replacing binary");
    if (!replaceBinary(tmp_path)) {
        fs::remove(tmp_path);
        rollback(backup_path);
        publishProgress(command_id, "rollback", "", "Binary replacement failed — rolled back");
        publishResult(command_id, "failed", "", "Binary replacement failed");
        return;
    }

    // Step 6: restart service
    publishProgress(command_id, "restarting", "Restarting service");
    if (!restartService()) {
        rollback(backup_path);
        publishProgress(command_id, "rollback", "", "Service restart failed — rolled back");
        publishResult(command_id, "failed", "", "Service restart failed, rolled back");
        return;
    }

    publishProgress(command_id, "success", "Updated to " + version + " successfully");
    publishResult(command_id, "success", "Updated to " + version + " successfully", "");
    spdlog::info("UpdateManager: update to {} completed", version);
}

// ---------------------------------------------------------------------------
// publishResult
// ---------------------------------------------------------------------------

void UpdateManager::publishResult(const std::string& command_id,
                                  const std::string& statut,
                                  const std::string& output,
                                  const std::string& error)
{
    json j;
    j["command_id"] = command_id;
    j["device_id"]  = device_id_;
    j["statut"]     = statut;
    j["output"]     = output.empty() ? json(nullptr) : json(output);
    j["error"]      = error.empty()  ? json(nullptr) : json(error);
    j["exit_code"]  = nullptr;
    j["timestamp"]  = nowISO8601();

    std::string topic = constants::topicFor(constants::TOPIC_RESULTS_FMT, device_id_);
    publish_fn_(topic, j.dump());
}

// ---------------------------------------------------------------------------
// publishProgress — étapes intermédiaires sur update_progress
// ---------------------------------------------------------------------------

void UpdateManager::publishProgress(const std::string& command_id,
                                    const std::string& step,
                                    const std::string& output,
                                    const std::string& error)
{
    json j;
    j["command_id"] = command_id;
    j["device_id"]  = device_id_;
    j["statut"]     = "executing";
    j["step"]       = step;
    j["output"]     = output.empty() ? json(nullptr) : json(output);
    j["error"]      = error.empty()  ? json(nullptr) : json(error);
    j["timestamp"]  = nowISO8601();

    std::string topic = constants::topicFor(constants::TOPIC_UPDATE_PROGRESS_FMT, device_id_);
    publish_fn_(topic, j.dump());
    spdlog::debug("UpdateManager: progress step={} cmd={}", step, command_id);
}

// ---------------------------------------------------------------------------
// Download via libcurl
// ---------------------------------------------------------------------------

bool UpdateManager::downloadBinary(const std::string& url, const std::string& dest) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    struct CurlGuard { CURL* c; ~CurlGuard() { curl_easy_cleanup(c); } } guard{curl};

    std::ofstream file(dest, std::ios::binary | std::ios::trunc);
    if (!file) return false;

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &file);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                     static_cast<long>(constants::TIMEOUT_AGENT_UPDATE_SEC));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        spdlog::error("UpdateManager: curl error: {}", curl_easy_strerror(rc));
        return false;
    }
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    return (http_code == 200);
}

// ---------------------------------------------------------------------------
// SHA-256 verification via OpenSSL
// ---------------------------------------------------------------------------

bool UpdateManager::verifySHA256(const std::string& file_path,
                                  const std::string& expected_hex)
{
    std::ifstream f(file_path, std::ios::binary);
    if (!f) return false;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return false;
    struct CtxGuard { EVP_MD_CTX* p; ~CtxGuard() { EVP_MD_CTX_free(p); } } cg{ctx};

    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);

    char buf[65536];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        EVP_DigestUpdate(ctx, buf, static_cast<size_t>(f.gcount()));
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx, digest, &len);

    // Convert to hex
    std::ostringstream oss;
    for (unsigned int i = 0; i < len; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(digest[i]);

    bool ok = (oss.str() == expected_hex);
    if (!ok) {
        spdlog::error("UpdateManager: SHA256 mismatch. expected={} got={}",
                      expected_hex, oss.str());
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Atomic binary replacement
// ---------------------------------------------------------------------------

bool UpdateManager::replaceBinary(const std::string& new_path) {
    std::string current = currentBinaryPath();

#if defined(PLATFORM_WINDOWS)
    // MoveFileEx is atomic on same volume
    if (!MoveFileExA(new_path.c_str(), current.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        spdlog::error("UpdateManager: MoveFileEx failed: {}", GetLastError());
        return false;
    }
#else
    // POSIX rename() is atomic on same filesystem
    if (rename(new_path.c_str(), current.c_str()) != 0) {
        spdlog::error("UpdateManager: rename failed");
        return false;
    }
    // Ensure executable bit
    fs::permissions(current,
                    fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add);
#endif
    return true;
}

// ---------------------------------------------------------------------------
// Restart service
// ---------------------------------------------------------------------------

bool UpdateManager::restartService() {
#if defined(PLATFORM_LINUX)
    int rc = system("systemctl restart orbis-agent");
    return rc == 0;
#elif defined(PLATFORM_MACOS)
    std::string cmd = std::string("launchctl unload ") + constants::LAUNCHD_PLIST_PATH +
                      " && launchctl load " + constants::LAUNCHD_PLIST_PATH;
    return system(cmd.c_str()) == 0;
#elif defined(PLATFORM_WINDOWS)
    return (system("net stop orbis-agent & net start orbis-agent") == 0);
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// Rollback
// ---------------------------------------------------------------------------

bool UpdateManager::rollback(const std::string& backup_path) {
    std::string current = currentBinaryPath();
    try {
        fs::copy_file(backup_path, current, fs::copy_options::overwrite_existing);
#if !defined(PLATFORM_WINDOWS)
        fs::permissions(current,
                        fs::perms::owner_exec | fs::perms::group_exec,
                        fs::perm_options::add);
#endif
        spdlog::warn("UpdateManager: rolled back to backup");
        return true;
    } catch (const std::exception& e) {
        spdlog::error("UpdateManager: rollback failed: {}", e.what());
        return false;
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string UpdateManager::currentBinaryPath() {
#if defined(PLATFORM_LINUX)
    char path[4096] = {};
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    return (len > 0) ? std::string(path, len) : constants::BINARY_INSTALL_PATH;
#elif defined(PLATFORM_MACOS)
    char path[4096] = {};
    uint32_t len = sizeof(path);
    _NSGetExecutablePath(path, &len);
    return path;
#elif defined(PLATFORM_WINDOWS)
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    return path;
#else
    return constants::BINARY_INSTALL_PATH;
#endif
}

std::string UpdateManager::nowISO8601() {
    auto now   = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now_t));
    return buf;
}

} // namespace orbis
