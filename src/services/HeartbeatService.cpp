#include "HeartbeatService.hpp"
#include "constants.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Platform includes
#if defined(PLATFORM_WINDOWS)
#  include <windows.h>
#  include <pdh.h>
#  include <iphlpapi.h>
#  pragma comment(lib, "pdh.lib")
#  pragma comment(lib, "iphlpapi.lib")
#elif defined(PLATFORM_MACOS)
#  include <sys/types.h>
#  include <sys/sysctl.h>
#  include <sys/statvfs.h>
#  include <sys/ioctl.h>
#  include <mach/mach.h>
#  include <unistd.h>
#  include <ifaddrs.h>
#  include <net/if.h>
#  include <net/if_media.h>
#  include <CoreFoundation/CoreFoundation.h>
#  include <IOKit/ps/IOPowerSources.h>
#  include <IOKit/ps/IOPSKeys.h>
#else  // Linux
#  include <sys/statvfs.h>
#  include <sys/utsname.h>
#  include <unistd.h>
#  include <dirent.h>
#endif

using json = nlohmann::json;

namespace orbis {

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

HeartbeatService::HeartbeatService(
    std::function<bool(const std::string&, const std::string&, int, bool)> publish_fn,
    const std::string& device_id,
    int interval_sec)
    : publish_fn_(std::move(publish_fn))
    , device_id_(device_id)
    , interval_sec_(interval_sec)
{}

HeartbeatService::~HeartbeatService() {
    stop();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void HeartbeatService::start() {
    if (running_) return;
    stop_requested_ = false;
    running_        = true;
    thread_         = std::thread(&HeartbeatService::run, this);
}

void HeartbeatService::stop() {
    stop_requested_ = true;
    if (thread_.joinable()) thread_.join();
    running_ = false;
}

bool HeartbeatService::isRunning() const { return running_; }

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void HeartbeatService::run() {
#if defined(PLATFORM_LINUX)
    pthread_setname_np(pthread_self(), "heartbeat");
#endif

    spdlog::info("HeartbeatService started (interval={}s)", interval_sec_);

    while (!stop_requested_) {
        try {
            std::string topic   = constants::topicFor(constants::TOPIC_STATUS_FMT, device_id_);
            std::string payload = buildPayload("online");
            if (!publish_fn_(topic, payload, constants::MQTT_DEFAULT_QOS, true)) {
                spdlog::warn("HeartbeatService: publish failed, will retry");
            }
        } catch (const std::exception& e) {
            spdlog::error("HeartbeatService: exception: {}", e.what());
        }

        // Sleep in 1s increments so stop() is responsive
        for (int i = 0; i < interval_sec_ && !stop_requested_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    // Publish final "offline" status on clean shutdown
    try {
        std::string topic   = constants::topicFor(constants::TOPIC_STATUS_FMT, device_id_);
        std::string payload = buildPayload("offline");
        publish_fn_(topic, payload, constants::MQTT_DEFAULT_QOS, true);
        spdlog::info("HeartbeatService: published offline status");
    } catch (...) {}

    spdlog::info("HeartbeatService stopped");
}

// ---------------------------------------------------------------------------
// Payload builder — Section 3 desktop format
// ---------------------------------------------------------------------------

std::string HeartbeatService::buildPayload(const std::string& status) const {
    auto now    = std::chrono::system_clock::now();
    auto now_t  = std::chrono::system_clock::to_time_t(now);
    char ts[32] = {};
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now_t));

    json j;
    j["device_id"]   = device_id_;
    j["timestamp"]   = ts;
    j["status"]      = status;
    j["version"]     = constants::AGENT_VERSION;
    j["uptime_sec"]  = getUptimeSec();
    j["hostname"]    = getHostname();
    j["plateforme"]  = getPlatform();
    j["os_version"]  = getOsVersion();
    j["architecture"]= getArchitecture();

    if (status == "online") {
        j["cpu_percent"]   = getCpuPercent();
        j["ram_percent"]   = getRamPercent();
        j["disk_percent"]  = getDiskPercent();
        j["network_type"]  = getNetworkType();

        int bat = getBatteryLevel();
        if (bat >= 0) {
            j["battery_level"]    = bat;
            j["battery_charging"] = isBatteryCharging();
        }
    }

    return j.dump();
}

// ---------------------------------------------------------------------------
// System metrics — Linux
// ---------------------------------------------------------------------------
#if defined(PLATFORM_LINUX)

double HeartbeatService::getCpuPercent() {
    // Read /proc/stat twice with a 500ms gap and compute delta
    auto readStat = []() -> std::array<unsigned long long, 4> {
        std::ifstream f("/proc/stat");
        std::string cpu;
        unsigned long long user, nice, system, idle;
        f >> cpu >> user >> nice >> system >> idle;
        return {user + nice, system, idle, user + nice + system + idle};
    };

    auto s1 = readStat();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto s2 = readStat();

    unsigned long long total_delta = s2[3] - s1[3];
    unsigned long long idle_delta  = s2[2] - s1[2];
    if (total_delta == 0) return 0.0;
    return 100.0 * (1.0 - static_cast<double>(idle_delta) / static_cast<double>(total_delta));
}

double HeartbeatService::getRamPercent() {
    std::ifstream f("/proc/meminfo");
    unsigned long long total = 0, available = 0;
    std::string key;
    unsigned long long val;
    std::string unit;
    while (f >> key >> val >> unit) {
        if (key == "MemTotal:")     total     = val;
        if (key == "MemAvailable:") available = val;
        if (total && available) break;
    }
    if (total == 0) return 0.0;
    return 100.0 * (1.0 - static_cast<double>(available) / static_cast<double>(total));
}

double HeartbeatService::getDiskPercent(const std::string& path) {
    struct statvfs sv{};
    if (statvfs(path.c_str(), &sv) != 0) return 0.0;
    if (sv.f_blocks == 0) return 0.0;
    unsigned long long total = sv.f_blocks * sv.f_frsize;
    unsigned long long free  = sv.f_bfree  * sv.f_frsize;
    return 100.0 * (1.0 - static_cast<double>(free) / static_cast<double>(total));
}

std::string HeartbeatService::getHostname() {
    char buf[256] = {};
    gethostname(buf, sizeof(buf) - 1);
    return buf;
}

std::string HeartbeatService::getPlatform() { return "linux"; }

std::string HeartbeatService::getOsVersion() {
    std::ifstream f("/etc/os-release");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            auto val = line.substr(12);
            if (!val.empty() && val.front() == '"') val = val.substr(1);
            if (!val.empty() && val.back()  == '"') val.pop_back();
            return val;
        }
    }
    return "Linux";
}

std::string HeartbeatService::getArchitecture() {
    struct utsname u{};
    uname(&u);
    return u.machine;
}

long long HeartbeatService::getUptimeSec() {
    std::ifstream f("/proc/uptime");
    double uptime = 0;
    f >> uptime;
    return static_cast<long long>(uptime);
}

int HeartbeatService::getBatteryLevel() {
    // Walk /sys/class/power_supply/ and look for a battery entry
    DIR* dir = opendir("/sys/class/power_supply");
    if (!dir) return -1;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        std::string type_path = "/sys/class/power_supply/" + name + "/type";
        std::ifstream tf(type_path);
        std::string type;
        if (tf >> type && type == "Battery") {
            std::ifstream cf("/sys/class/power_supply/" + name + "/capacity");
            int cap = -1;
            cf >> cap;
            closedir(dir);
            return cap;
        }
    }
    closedir(dir);
    return -1;  // no battery (desktop/server)
}

bool HeartbeatService::isBatteryCharging() {
    DIR* dir = opendir("/sys/class/power_supply");
    if (!dir) return false;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        std::string type_path = "/sys/class/power_supply/" + name + "/type";
        std::ifstream tf(type_path);
        std::string type;
        if (tf >> type && type == "Battery") {
            std::ifstream sf("/sys/class/power_supply/" + name + "/status");
            std::string status;
            sf >> status;
            closedir(dir);
            return (status == "Charging" || status == "Full");
        }
    }
    closedir(dir);
    return false;
}

std::string HeartbeatService::getNetworkType() {
    // Wireless: any active entry in /proc/net/wireless
    {
        std::ifstream wf("/proc/net/wireless");
        std::string line;
        int n = 0;
        while (std::getline(wf, line)) {
            // First 2 lines are headers
            if (n++ >= 2 && line.find_first_not_of(" \t") != std::string::npos)
                return "WIFI";
        }
    }
    // Wired: first non-loopback, non-wireless interface with operstate=up
    DIR* dir = opendir("/sys/class/net");
    if (dir) {
        struct dirent* e;
        while ((e = readdir(dir)) != nullptr) {
            std::string iface = e->d_name;
            if (iface == "." || iface == ".." || iface == "lo") continue;
            // wlan*, wlp*, wl* are wireless
            if (iface.substr(0, 2) == "wl") continue;
            std::ifstream os("/sys/class/net/" + iface + "/operstate");
            std::string state;
            if (os >> state && state == "up") {
                closedir(dir);
                return "ETHERNET";
            }
        }
        closedir(dir);
    }
    return "NONE";
}

// ---------------------------------------------------------------------------
// System metrics — macOS
// ---------------------------------------------------------------------------
#elif defined(PLATFORM_MACOS)

double HeartbeatService::getCpuPercent() {
    // Use host_processor_info with two samples
    auto sample = []() -> std::pair<uint64_t,uint64_t> {
        processor_info_array_t info;
        mach_msg_type_number_t count;
        natural_t num_cpus;
        host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO,
                            &num_cpus, &info, &count);
        uint64_t user=0, sys=0, idle=0, nice=0;
        for (natural_t i = 0; i < num_cpus; ++i) {
            processor_cpu_load_info_t load = (processor_cpu_load_info_t)info + i;
            user += load->cpu_ticks[CPU_STATE_USER];
            sys  += load->cpu_ticks[CPU_STATE_SYSTEM];
            idle += load->cpu_ticks[CPU_STATE_IDLE];
            nice += load->cpu_ticks[CPU_STATE_NICE];
        }
        vm_deallocate(mach_task_self(), (vm_address_t)info, count * sizeof(*info));
        return {user + sys + nice, user + sys + nice + idle};
    };
    auto [busy1, total1] = sample();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto [busy2, total2] = sample();
    if (total2 == total1) return 0.0;
    return 100.0 * static_cast<double>(busy2 - busy1) / static_cast<double>(total2 - total1);
}

double HeartbeatService::getRamPercent() {
    vm_statistics64_data_t vm_stat{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm_stat, &count);
    uint64_t page = getpagesize();
    uint64_t free = (vm_stat.free_count + vm_stat.inactive_count) * page;
    int64_t total_bytes = 0;
    size_t len = sizeof(total_bytes);
    sysctlbyname("hw.memsize", &total_bytes, &len, nullptr, 0);
    if (total_bytes == 0) return 0.0;
    return 100.0 * (1.0 - static_cast<double>(free) / static_cast<double>(total_bytes));
}

double HeartbeatService::getDiskPercent(const std::string& path) {
    struct statvfs sv{};
    if (statvfs(path.c_str(), &sv) != 0) return 0.0;
    if (sv.f_blocks == 0) return 0.0;
    return 100.0 * (1.0 - static_cast<double>(sv.f_bfree) / static_cast<double>(sv.f_blocks));
}

std::string HeartbeatService::getHostname() {
    char buf[256] = {};
    gethostname(buf, sizeof(buf) - 1);
    return buf;
}

std::string HeartbeatService::getPlatform() { return "macos"; }

std::string HeartbeatService::getOsVersion() {
    char buf[64] = {};
    size_t len = sizeof(buf);
    sysctlbyname("kern.osproductversion", buf, &len, nullptr, 0);
    return std::string("macOS ") + buf;
}

std::string HeartbeatService::getArchitecture() {
    char buf[32] = {};
    size_t len = sizeof(buf);
    sysctlbyname("hw.machine", buf, &len, nullptr, 0);
    return buf;
}

long long HeartbeatService::getUptimeSec() {
    struct timeval boot{};
    size_t len = sizeof(boot);
    sysctlbyname("kern.boottime", &boot, &len, nullptr, 0);
    return static_cast<long long>(time(nullptr) - boot.tv_sec);
}

int HeartbeatService::getBatteryLevel() {
    CFTypeRef blob = IOPSCopyPowerSourcesInfo();
    if (!blob) return -1;
    CFArrayRef sources = IOPSCopyPowerSourcesList(blob);
    if (!sources) { CFRelease(blob); return -1; }

    int level = -1;
    CFIndex count = CFArrayGetCount(sources);
    for (CFIndex i = 0; i < count && level < 0; ++i) {
        CFTypeRef src = CFArrayGetValueAtIndex(sources, i);
        CFDictionaryRef desc = IOPSGetPowerSourceDescription(blob, src);
        if (!desc) continue;

        CFStringRef type = (CFStringRef)CFDictionaryGetValue(desc, CFSTR(kIOPSTypeKey));
        if (!type || CFStringCompare(type, CFSTR(kIOPSInternalBatteryType), 0) != kCFCompareEqualTo)
            continue;

        CFNumberRef cap = (CFNumberRef)CFDictionaryGetValue(desc, CFSTR(kIOPSCurrentCapacityKey));
        CFNumberRef max = (CFNumberRef)CFDictionaryGetValue(desc, CFSTR(kIOPSMaxCapacityKey));
        if (cap && max) {
            int cur = 0, mx = 100;
            CFNumberGetValue(cap, kCFNumberIntType, &cur);
            CFNumberGetValue(max, kCFNumberIntType, &mx);
            if (mx > 0) level = (cur * 100) / mx;
        }
    }
    CFRelease(sources);
    CFRelease(blob);
    return level;
}

bool HeartbeatService::isBatteryCharging() {
    CFTypeRef blob = IOPSCopyPowerSourcesInfo();
    if (!blob) return false;
    CFArrayRef sources = IOPSCopyPowerSourcesList(blob);
    if (!sources) { CFRelease(blob); return false; }

    bool charging = false;
    CFIndex count = CFArrayGetCount(sources);
    for (CFIndex i = 0; i < count && !charging; ++i) {
        CFTypeRef src = CFArrayGetValueAtIndex(sources, i);
        CFDictionaryRef desc = IOPSGetPowerSourceDescription(blob, src);
        if (!desc) continue;

        CFStringRef type = (CFStringRef)CFDictionaryGetValue(desc, CFSTR(kIOPSTypeKey));
        if (!type || CFStringCompare(type, CFSTR(kIOPSInternalBatteryType), 0) != kCFCompareEqualTo)
            continue;

        CFStringRef state = (CFStringRef)CFDictionaryGetValue(desc, CFSTR(kIOPSPowerSourceStateKey));
        if (state && CFStringCompare(state, CFSTR(kIOPSACPowerValue), 0) == kCFCompareEqualTo)
            charging = true;
    }
    CFRelease(sources);
    CFRelease(blob);
    return charging;
}

std::string HeartbeatService::getNetworkType() {
    // Walk active interfaces; use SIOCGIFMEDIA to detect IEEE 802.11 (WiFi)
    struct ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) != 0) return "NONE";

    std::string result = "NONE";
    for (struct ifaddrs* ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;  // IPv4 only

        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) continue;

        struct ifmediareq ifmr{};
        strlcpy(ifmr.ifm_name, ifa->ifa_name, sizeof(ifmr.ifm_name));
        if (ioctl(sock, SIOCGIFMEDIA, &ifmr) == 0) {
            result = (IFM_TYPE(ifmr.ifm_active) == IFM_IEEE80211) ? "WIFI" : "ETHERNET";
            close(sock);
            break;
        }
        close(sock);
    }
    freeifaddrs(ifap);
    return result;
}

// ---------------------------------------------------------------------------
// System metrics — Windows
// ---------------------------------------------------------------------------
#elif defined(PLATFORM_WINDOWS)

double HeartbeatService::getCpuPercent() {
    FILETIME idle1, kernel1, user1, idle2, kernel2, user2;
    GetSystemTimes(&idle1, &kernel1, &user1);
    Sleep(500);
    GetSystemTimes(&idle2, &kernel2, &user2);
    auto toULL = [](FILETIME ft) -> ULONGLONG {
        return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    ULONGLONG idle   = toULL(idle2)   - toULL(idle1);
    ULONGLONG kernel = toULL(kernel2) - toULL(kernel1);
    ULONGLONG user   = toULL(user2)   - toULL(user1);
    ULONGLONG total  = kernel + user;
    if (total == 0) return 0.0;
    return 100.0 * (1.0 - static_cast<double>(idle) / static_cast<double>(total));
}

double HeartbeatService::getRamPercent() {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    return static_cast<double>(ms.dwMemoryLoad);
}

double HeartbeatService::getDiskPercent(const std::string& /*path*/) {
    ULARGE_INTEGER free_bytes, total_bytes, total_free;
    if (!GetDiskFreeSpaceExA("C:\\", &free_bytes, &total_bytes, &total_free)) return 0.0;
    if (total_bytes.QuadPart == 0) return 0.0;
    return 100.0 * (1.0 - static_cast<double>(free_bytes.QuadPart)
                          / static_cast<double>(total_bytes.QuadPart));
}

std::string HeartbeatService::getHostname() {
    char buf[256] = {};
    DWORD len = sizeof(buf);
    GetComputerNameA(buf, &len);
    return buf;
}

std::string HeartbeatService::getPlatform() { return "windows"; }

std::string HeartbeatService::getOsVersion() {
    OSVERSIONINFOEX info{};
    info.dwOSVersionInfoSize = sizeof(info);
    GetVersionEx(reinterpret_cast<OSVERSIONINFO*>(&info));
    return "Windows " + std::to_string(info.dwMajorVersion) + "." +
           std::to_string(info.dwMinorVersion);
}

std::string HeartbeatService::getArchitecture() {
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: return "x86_64";
        case PROCESSOR_ARCHITECTURE_ARM64: return "arm64";
        case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
        default: return "unknown";
    }
}

long long HeartbeatService::getUptimeSec() {
    return static_cast<long long>(GetTickCount64() / 1000ULL);
}

int HeartbeatService::getBatteryLevel() {
    SYSTEM_POWER_STATUS sps{};
    if (!GetSystemPowerStatus(&sps)) return -1;
    if (sps.BatteryLifePercent == 255) return -1;  // no battery
    return static_cast<int>(sps.BatteryLifePercent);
}

bool HeartbeatService::isBatteryCharging() {
    SYSTEM_POWER_STATUS sps{};
    if (!GetSystemPowerStatus(&sps)) return false;
    // ACLineStatus: 0=battery, 1=AC, 255=unknown
    return sps.ACLineStatus == 1;
}

std::string HeartbeatService::getNetworkType() {
    ULONG bufLen = 15000;
    std::vector<BYTE> buf(bufLen);
    auto* addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr,
                              addrs, &bufLen) != NO_ERROR)
        return "NONE";
    for (auto* a = addrs; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (a->IfType == IF_TYPE_IEEE80211)       return "WIFI";
        if (a->IfType == IF_TYPE_ETHERNET_CSMACD) return "ETHERNET";
    }
    return "NONE";
}

#endif // Platform

} // namespace orbis
