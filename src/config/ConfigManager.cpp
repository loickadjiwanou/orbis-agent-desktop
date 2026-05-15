#include "ConfigManager.hpp"
#include "constants.hpp"

#include <toml.hpp>
#include <spdlog/spdlog.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/rand.h>

#include <array>
#include <cstring>
#include <fstream>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>

#if defined(PLATFORM_WINDOWS)
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace orbis {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Simple base64 encode/decode (no external dependency)
static std::string b64Encode(const std::vector<unsigned char>& data) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        unsigned char b0 = data[i];
        unsigned char b1 = (i + 1 < data.size()) ? data[i + 1] : 0;
        unsigned char b2 = (i + 2 < data.size()) ? data[i + 2] : 0;
        out.push_back(tbl[b0 >> 2]);
        out.push_back(tbl[((b0 & 3) << 4) | (b1 >> 4)]);
        out.push_back((i + 1 < data.size()) ? tbl[((b1 & 15) << 2) | (b2 >> 6)] : '=');
        out.push_back((i + 2 < data.size()) ? tbl[b2 & 63] : '=');
    }
    return out;
}

static std::vector<unsigned char> b64Decode(const std::string& in) {
    static const int lookup[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    };
    std::vector<unsigned char> out;
    out.reserve(in.size() * 3 / 4);
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c >= 128 || lookup[c] == -1) break;
        val = (val << 6) + lookup[c];
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<unsigned char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

/// Generate UUID v4
static std::string generateUUID() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t hi = dist(gen);
    uint64_t lo = dist(gen);
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    char buf[37];
    snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
             static_cast<uint32_t>(hi >> 32),
             static_cast<uint16_t>(hi >> 16),
             static_cast<uint16_t>(hi),
             static_cast<uint16_t>(lo >> 48),
             static_cast<unsigned long long>(lo & 0x0000FFFFFFFFFFFFULL));
    return buf;
}

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

ConfigManager& ConfigManager::instance() {
    static ConfigManager inst;
    return inst;
}

// ---------------------------------------------------------------------------
// Key derivation from hostname
// ---------------------------------------------------------------------------

std::vector<unsigned char> ConfigManager::deriveKey() {
    char hostname[256] = {};
#if defined(PLATFORM_WINDOWS)
    DWORD len = sizeof(hostname);
    GetComputerNameA(hostname, &len);
#else
    gethostname(hostname, sizeof(hostname) - 1);
#endif
    // SHA-256 of hostname → 32 bytes key
    std::vector<unsigned char> key(SHA256_DIGEST_LENGTH);
    SHA256(reinterpret_cast<const unsigned char*>(hostname),
           strlen(hostname), key.data());
    return key;
}

// ---------------------------------------------------------------------------
// AES-256-CBC encrypt/decrypt
// ---------------------------------------------------------------------------

std::string ConfigManager::encryptToken(const std::string& plaintext) {
    if (plaintext.empty()) return "";
    auto key = deriveKey();

    // Random IV (16 bytes)
    std::vector<unsigned char> iv(16);
    if (RAND_bytes(iv.data(), 16) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");

    struct CtxGuard {
        EVP_CIPHER_CTX* p;
        ~CtxGuard() { EVP_CIPHER_CTX_free(p); }
    } guard{ctx};

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr,
                           key.data(), iv.data()) != 1)
        throw std::runtime_error("EVP_EncryptInit_ex failed");

    std::vector<unsigned char> ciphertext(plaintext.size() + 16);
    int outlen = 0, finlen = 0;

    if (EVP_EncryptUpdate(ctx, ciphertext.data(), &outlen,
                          reinterpret_cast<const unsigned char*>(plaintext.data()),
                          static_cast<int>(plaintext.size())) != 1)
        throw std::runtime_error("EVP_EncryptUpdate failed");

    if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + outlen, &finlen) != 1)
        throw std::runtime_error("EVP_EncryptFinal_ex failed");

    ciphertext.resize(outlen + finlen);

    // Format: base64(IV || ciphertext)
    std::vector<unsigned char> combined(iv.begin(), iv.end());
    combined.insert(combined.end(), ciphertext.begin(), ciphertext.end());
    return b64Encode(combined);
}

std::string ConfigManager::decryptToken(const std::string& ciphertext_b64) {
    if (ciphertext_b64.empty()) return "";
    auto key = deriveKey();
    auto combined = b64Decode(ciphertext_b64);
    if (combined.size() < 17) return ""; // too short

    std::vector<unsigned char> iv(combined.begin(), combined.begin() + 16);
    std::vector<unsigned char> ciphertext(combined.begin() + 16, combined.end());

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return "";
    struct CtxGuard {
        EVP_CIPHER_CTX* p;
        ~CtxGuard() { EVP_CIPHER_CTX_free(p); }
    } guard{ctx};

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr,
                           key.data(), iv.data()) != 1) return "";

    std::vector<unsigned char> plaintext(ciphertext.size() + 16);
    int outlen = 0, finlen = 0;

    if (EVP_DecryptUpdate(ctx, plaintext.data(), &outlen,
                          ciphertext.data(),
                          static_cast<int>(ciphertext.size())) != 1) return "";

    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + outlen, &finlen) != 1) return "";

    return std::string(reinterpret_cast<char*>(plaintext.data()), outlen + finlen);
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

bool ConfigManager::load(const std::string& config_path) {
    std::lock_guard<std::mutex> lock(mu_);
    config_path_ = config_path;

    toml::value tbl;
    try {
        tbl = toml::parse(config_path);
    } catch (const std::exception& e) {
        spdlog::error("Config parse error: {}", e.what());
        return false;
    }

    // [agent]
    device_id_       = toml::find_or(tbl, "agent", "device_id", std::string{});
    token_encrypted_ = toml::find_or(tbl, "agent", "token",     std::string{});
    log_file_        = toml::find_or(tbl, "agent", "log_file",  std::string{constants::DEFAULT_LOG_PATH});
    buffer_db_       = toml::find_or(tbl, "agent", "buffer_db", std::string{constants::DEFAULT_BUFFER_PATH});

    // Auto-generate device_id if missing
    if (device_id_.empty()) {
        device_id_ = generateUUID();
        spdlog::info("Generated new device_id: {}", device_id_);
    }

    // [mqtt]
    mqtt_.broker_host           = toml::find_or(tbl, "mqtt", "broker_host",           std::string{"localhost"});
    mqtt_.broker_port           = toml::find_or(tbl, "mqtt", "broker_port",           8883);
    mqtt_.use_tls               = toml::find_or(tbl, "mqtt", "use_tls",               true);
    mqtt_.ca_cert_path          = toml::find_or(tbl, "mqtt", "ca_cert_path",          std::string{});
    mqtt_.register_secret       = toml::find_or(tbl, "mqtt", "register_secret",       std::string{"orbis_register_secret"});
    mqtt_.reconnect_delay_min_sec = toml::find_or(tbl, "mqtt", "reconnect_delay_min_sec", 1);
    mqtt_.reconnect_delay_max_sec = toml::find_or(tbl, "mqtt", "reconnect_delay_max_sec", 60);
    mqtt_.keepalive_sec         = toml::find_or(tbl, "mqtt", "keepalive_sec",         60);

    // [logs]
    log_.interval_sec = toml::find_or(tbl, "logs", "interval_sec", 60);
    if (tbl.contains("logs") && tbl.at("logs").contains("levels")) {
        log_.levels.clear();
        for (auto& v : toml::find<toml::array>(tbl, "logs", "levels"))
            log_.levels.push_back(toml::get<std::string>(v));
    }
    if (tbl.contains("logs") && tbl.at("logs").contains("sources")) {
        log_.sources.clear();
        for (auto& v : toml::find<toml::array>(tbl, "logs", "sources"))
            log_.sources.push_back(toml::get<std::string>(v));
    }

    // [heartbeat]
    heartbeat_.interval_sec = toml::find_or(tbl, "heartbeat", "interval_sec", 30);

    // [plugins]
    plugins_.enabled     = toml::find_or(tbl, "plugins", "enabled",     true);
    plugins_.plugins_dir = toml::find_or(tbl, "plugins", "plugins_dir", std::string{"./plugins"});

    // [network_scanner]
    scanner_.enabled           = toml::find_or(tbl, "network_scanner", "enabled",           false);
    scanner_.scan_interval_sec = toml::find_or(tbl, "network_scanner", "scan_interval_sec", 300);
    scanner_.port_range        = toml::find_or(tbl, "network_scanner", "port_range",        std::string{"22,80,443,8080,8883"});

    spdlog::info("Config loaded from {}", config_path);
    return true;
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

bool ConfigManager::save() {
    std::lock_guard<std::mutex> lock(mu_);

    // Read existing file, patch the two managed fields, rewrite.
    std::ifstream fin(config_path_);
    if (!fin) {
        spdlog::error("Cannot open config for saving: {}", config_path_);
        return false;
    }
    std::ostringstream buf;
    buf << fin.rdbuf();
    fin.close();

    std::string content = buf.str();

    auto replace = [&](const std::string& key, const std::string& value) {
        std::regex re(key + R"(\s*=\s*"[^"]*")");
        content = std::regex_replace(content, re, key + " = \"" + value + "\"");
    };

    replace("device_id", device_id_);
    replace("token",     token_encrypted_);

    std::ofstream fout(config_path_);
    if (!fout) {
        spdlog::error("Cannot write config: {}", config_path_);
        return false;
    }
    fout << content;
    spdlog::info("Config saved to {}", config_path_);
    return true;
}

// ---------------------------------------------------------------------------
// Getters / setters
// ---------------------------------------------------------------------------

std::string ConfigManager::getDeviceId() const {
    std::lock_guard<std::mutex> lock(mu_);
    return device_id_;
}

void ConfigManager::setDeviceId(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    device_id_ = id;
}

std::string ConfigManager::getToken() const {
    std::lock_guard<std::mutex> lock(mu_);
    return decryptToken(token_encrypted_);
}

void ConfigManager::setToken(const std::string& token) {
    std::lock_guard<std::mutex> lock(mu_);
    token_encrypted_ = encryptToken(token);
}

bool ConfigManager::isRegistered() const {
    std::lock_guard<std::mutex> lock(mu_);
    return !device_id_.empty() && !token_encrypted_.empty();
}

MQTTConfig ConfigManager::getMQTTConfig() const {
    std::lock_guard<std::mutex> lock(mu_);
    return mqtt_;
}

LogConfig ConfigManager::getLogConfig() const {
    std::lock_guard<std::mutex> lock(mu_);
    return log_;
}

HeartbeatConfig ConfigManager::getHeartbeatConfig() const {
    std::lock_guard<std::mutex> lock(mu_);
    return heartbeat_;
}

PluginConfig ConfigManager::getPluginConfig() const {
    std::lock_guard<std::mutex> lock(mu_);
    return plugins_;
}

NetworkScannerConfig ConfigManager::getNetworkScannerConfig() const {
    std::lock_guard<std::mutex> lock(mu_);
    return scanner_;
}

std::string ConfigManager::getLogFilePath() const {
    std::lock_guard<std::mutex> lock(mu_);
    return log_file_;
}

std::string ConfigManager::getBufferDbPath() const {
    std::lock_guard<std::mutex> lock(mu_);
    return buffer_db_;
}

} // namespace orbis
