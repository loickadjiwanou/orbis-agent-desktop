#include "NetworkScanner.hpp"
#include "constants.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <future>
#include <sstream>
#include <stdexcept>
#include <thread>

#if defined(PLATFORM_WINDOWS)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#  pragma comment(lib, "ws2_32.lib")
#  pragma comment(lib, "iphlpapi.lib")
typedef int socklen_t;
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <ifaddrs.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

using json = nlohmann::json;

namespace orbis {

NetworkScanner::NetworkScanner(
    std::function<bool(const std::string&, const std::string&)> publish_fn,
    const std::string& device_id,
    int scan_interval_sec,
    const std::string& port_range)
    : publish_fn_(std::move(publish_fn))
    , device_id_(device_id)
    , scan_interval_sec_(scan_interval_sec)
    , port_range_(port_range)
{}

NetworkScanner::~NetworkScanner() { stop(); }

void NetworkScanner::start() {
    if (running_) return;
    stop_requested_ = false;
    running_        = true;
    thread_         = std::thread(&NetworkScanner::run, this);
}

void NetworkScanner::stop() {
    stop_requested_ = true;
    trigger_now_    = true;
    if (thread_.joinable()) thread_.join();
    running_ = false;
}

bool NetworkScanner::isRunning() const { return running_; }
void NetworkScanner::triggerNow() { trigger_now_ = true; }

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void NetworkScanner::run() {
#if defined(PLATFORM_LINUX)
    pthread_setname_np(pthread_self(), "net-scanner");
#endif
    spdlog::info("NetworkScanner started (interval={}s)", scan_interval_sec_);

    while (!stop_requested_) {
        try {
            scan();
        } catch (const std::exception& e) {
            spdlog::error("NetworkScanner: {}", e.what());
        }

        for (int i = 0; i < scan_interval_sec_ && !stop_requested_ && !trigger_now_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        trigger_now_ = false;
    }
    spdlog::info("NetworkScanner stopped");
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

void NetworkScanner::scan() {
    std::string cidr = detectLocalSubnet();
    if (cidr.empty()) {
        spdlog::warn("NetworkScanner: could not detect local subnet");
        return;
    }

    spdlog::info("NetworkScanner: scanning {}", cidr);
    auto hosts  = expandCIDR(cidr);
    auto ports  = parsePorts(port_range_);

    // Scan up to 254 hosts in parallel (thread pool of 32)
    std::vector<Neighbor> neighbors;
    std::mutex            nbr_mutex;

    size_t concurrency = 32;
    std::vector<std::future<void>> tasks;

    for (auto& ip : hosts) {
        if (stop_requested_) break;

        // Limit concurrency
        if (tasks.size() >= concurrency) {
            tasks.front().wait();
            tasks.erase(tasks.begin());
        }

        tasks.push_back(std::async(std::launch::async, [&, ip]() {
            int resp_ms = 0;
            if (!pingHost(ip, resp_ms)) return;

            Neighbor n;
            n.ip               = ip;
            n.response_time_ms = resp_ms;
            n.hostname         = resolveHostname(ip);

            for (int port : ports) {
                if (probePort(ip, port)) n.open_ports.push_back(port);
            }

            std::lock_guard<std::mutex> lock(nbr_mutex);
            neighbors.push_back(std::move(n));
        }));
    }
    for (auto& t : tasks) t.wait();

    // Build and publish discovery payload (Section 8)
    json j;
    j["device_id"] = device_id_;
    j["timestamp"] = nowISO8601();
    j["network"]   = cidr;
    j["neighbors"] = json::array();

    for (auto& n : neighbors) {
        json nb;
        nb["ip"]               = n.ip;
        nb["hostname"]         = n.hostname;
        nb["open_ports"]       = n.open_ports;
        nb["response_time_ms"] = n.response_time_ms;
        j["neighbors"].push_back(nb);
    }

    std::string topic = constants::topicFor(constants::TOPIC_DISCOVERY_FMT, device_id_);
    if (!publish_fn_(topic, j.dump())) {
        spdlog::warn("NetworkScanner: publish failed");
    } else {
        spdlog::info("NetworkScanner: published {} neighbors", neighbors.size());
    }
}

// ---------------------------------------------------------------------------
// Detect local subnet (returns CIDR like "192.168.1.0/24")
// ---------------------------------------------------------------------------

std::string NetworkScanner::detectLocalSubnet() {
#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
    struct ifaddrs* ifa_list = nullptr;
    if (getifaddrs(&ifa_list) != 0) return "";
    struct IfGuard { ifaddrs* p; ~IfGuard() { freeifaddrs(p); } } guard{ifa_list};

    for (auto* ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (std::string(ifa->ifa_name) == "lo") continue;

        auto* addr = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
        auto* mask = reinterpret_cast<sockaddr_in*>(ifa->ifa_netmask);
        uint32_t ip_n  = ntohl(addr->sin_addr.s_addr);
        uint32_t msk_n = ntohl(mask->sin_addr.s_addr);
        uint32_t net_n = ip_n & msk_n;

        // Count prefix bits
        int prefix = 0;
        uint32_t m = msk_n;
        while (m & 0x80000000) { prefix++; m <<= 1; }

        char net_s[INET_ADDRSTRLEN];
        uint32_t net_be = htonl(net_n);
        inet_ntop(AF_INET, &net_be, net_s, sizeof(net_s));
        return std::string(net_s) + "/" + std::to_string(prefix);
    }
    return "";

#elif defined(PLATFORM_WINDOWS)
    // Use GetAdaptersAddresses
    ULONG bufSize = 15000;
    std::vector<BYTE> buf(bufSize);
    auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, 0, nullptr, aa, &bufSize) != NO_ERROR) return "";
    for (auto* a = aa; a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
            auto* sin = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
            char ip_s[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sin->sin_addr, ip_s, sizeof(ip_s));
            int prefix = ua->OnLinkPrefixLength;
            uint32_t ip_n  = ntohl(sin->sin_addr.s_addr);
            uint32_t msk_n = (prefix > 0) ? (~0U << (32 - prefix)) : 0;
            uint32_t net_n = ip_n & msk_n;
            uint32_t net_be = htonl(net_n);
            char net_s[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &net_be, net_s, sizeof(net_s));
            return std::string(net_s) + "/" + std::to_string(prefix);
        }
    }
    return "";
#else
    return "";
#endif
}

// ---------------------------------------------------------------------------
// Expand CIDR → list of host IPs (/24 → 254 hosts max)
// ---------------------------------------------------------------------------

std::vector<std::string> NetworkScanner::expandCIDR(const std::string& cidr) {
    auto slash = cidr.find('/');
    if (slash == std::string::npos) return {};
    std::string net_s  = cidr.substr(0, slash);
    int prefix = std::stoi(cidr.substr(slash + 1));
    if (prefix < 16 || prefix > 30) return {}; // safety: don't scan huge ranges

    uint32_t net_n;
    inet_pton(AF_INET, net_s.c_str(), &net_n);
    net_n = ntohl(net_n);

    uint32_t host_bits = 32 - prefix;
    uint32_t count     = (1U << host_bits) - 2; // exclude network & broadcast
    if (count > 254) count = 254;

    std::vector<std::string> hosts;
    hosts.reserve(count);
    for (uint32_t i = 1; i <= count; ++i) {
        uint32_t host_be = htonl(net_n + i);
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &host_be, buf, sizeof(buf));
        hosts.emplace_back(buf);
    }
    return hosts;
}

// ---------------------------------------------------------------------------
// Ping (TCP connect to port 7/ECHO or ICMP — use TCP port 80/443 as fallback)
// ---------------------------------------------------------------------------

bool NetworkScanner::pingHost(const std::string& ip, int& response_ms) {
    // Use TCP connect to port 80 as "ping" (works without raw socket privileges)
    auto start = std::chrono::steady_clock::now();
    bool ok = probePort(ip, 80, 1000) || probePort(ip, 443, 1000) ||
              probePort(ip, 22, 1000);
    if (ok) {
        auto end = std::chrono::steady_clock::now();
        response_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Non-blocking TCP port probe
// ---------------------------------------------------------------------------

bool NetworkScanner::probePort(const std::string& ip, int port, int timeout_ms) {
#if defined(PLATFORM_WINDOWS)
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) return false;
    struct SockGuard { SOCKET s; ~SockGuard() { closesocket(s); } } sg{sock};

    // Set non-blocking
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, ip.c_str(), &sa.sin_addr);

    connect(sock, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    timeval tv{0, timeout_ms * 1000};
    int rc = select(0, nullptr, &wfds, nullptr, &tv);
    return rc > 0;

#else
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;
    struct SockGuard { int s; ~SockGuard() { close(s); } } sg{sock};

    // Set non-blocking
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, ip.c_str(), &sa.sin_addr);

    connect(sock, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    timeval tv{0, timeout_ms * 1000};
    int rc = select(sock + 1, nullptr, &wfds, nullptr, &tv);
    return rc > 0;
#endif
}

// ---------------------------------------------------------------------------
// Reverse DNS
// ---------------------------------------------------------------------------

std::string NetworkScanner::resolveHostname(const std::string& ip) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    inet_pton(AF_INET, ip.c_str(), &sa.sin_addr);
    char host[NI_MAXHOST] = {};
    if (getnameinfo(reinterpret_cast<sockaddr*>(&sa), sizeof(sa),
                    host, sizeof(host), nullptr, 0, NI_NAMEREQD) == 0) {
        return host;
    }
    return "";
}

// ---------------------------------------------------------------------------
// Parse port list "22,80,443,8080"
// ---------------------------------------------------------------------------

std::vector<int> NetworkScanner::parsePorts(const std::string& range) {
    std::vector<int> ports;
    std::istringstream ss(range);
    std::string token;
    while (std::getline(ss, token, ',')) {
        try { ports.push_back(std::stoi(token)); } catch (...) {}
    }
    return ports;
}

std::string NetworkScanner::nowISO8601() {
    auto now   = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now_t));
    return buf;
}

} // namespace orbis
