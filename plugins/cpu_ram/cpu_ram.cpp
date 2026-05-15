/**
 * @file cpu_ram.cpp
 * @brief Orbis plugin — CPU and RAM metrics.
 *
 * Returns a JSON array with one entry containing cpu_percent, ram_percent,
 * ram_used_mb, ram_total_mb.
 *
 * Platform support:
 *   Linux  : /proc/stat (CPU), /proc/meminfo (RAM)
 *   macOS  : host_processor_info, host_statistics64
 *   Windows: GetSystemTimes, GlobalMemoryStatusEx
 */
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#if defined(PLATFORM_WINDOWS)
#  include <windows.h>
#elif defined(PLATFORM_MACOS)
#  include <mach/mach.h>
#  include <sys/sysctl.h>
#  include <unistd.h>
#else
#  include <fstream>
#endif

using json = nlohmann::json;

// Keep previous CPU sample for delta computation
static double g_cpu_percent = 0.0;
static bool   g_initialized = false;

// ---------------------------------------------------------------------------
// CPU measurement helpers
// ---------------------------------------------------------------------------

#if defined(PLATFORM_LINUX)
struct CpuSample { unsigned long long idle, total; };

static CpuSample readCpuSample() {
    std::ifstream f("/proc/stat");
    std::string cpu;
    unsigned long long user, nice, system, idle, iowait = 0, irq = 0, softirq = 0;
    f >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq;
    return {idle + iowait, user + nice + system + idle + iowait + irq + softirq};
}

static double measureCpu() {
    auto s1 = readCpuSample();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto s2 = readCpuSample();
    auto total_delta = s2.total - s1.total;
    auto idle_delta  = s2.idle  - s1.idle;
    if (total_delta == 0) return 0.0;
    return 100.0 * (1.0 - static_cast<double>(idle_delta) /
                          static_cast<double>(total_delta));
}

static void measureRam(double& ram_pct, unsigned long long& used_mb,
                       unsigned long long& total_mb)
{
    std::ifstream f("/proc/meminfo");
    unsigned long long total_kb = 0, avail_kb = 0;
    std::string key; unsigned long long val; std::string unit;
    while (f >> key >> val >> unit) {
        if (key == "MemTotal:")     total_kb = val;
        if (key == "MemAvailable:") avail_kb = val;
        if (total_kb && avail_kb) break;
    }
    total_mb = total_kb / 1024;
    used_mb  = (total_kb - avail_kb) / 1024;
    ram_pct  = total_kb ? 100.0 * (1.0 - static_cast<double>(avail_kb) /
                                         static_cast<double>(total_kb)) : 0.0;
}

#elif defined(PLATFORM_MACOS)
static double measureCpu() {
    auto sample = []() -> std::pair<uint64_t,uint64_t> {
        processor_info_array_t info;
        mach_msg_type_number_t count;
        natural_t num_cpus;
        host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO,
                            &num_cpus, &info, &count);
        uint64_t busy = 0, total = 0;
        for (natural_t i = 0; i < num_cpus; ++i) {
            auto* ld = (processor_cpu_load_info_t)info + i;
            busy  += ld->cpu_ticks[CPU_STATE_USER] + ld->cpu_ticks[CPU_STATE_SYSTEM]
                   + ld->cpu_ticks[CPU_STATE_NICE];
            total += ld->cpu_ticks[CPU_STATE_USER] + ld->cpu_ticks[CPU_STATE_SYSTEM]
                   + ld->cpu_ticks[CPU_STATE_NICE] + ld->cpu_ticks[CPU_STATE_IDLE];
        }
        vm_deallocate(mach_task_self(), (vm_address_t)info, count * sizeof(*info));
        return {busy, total};
    };
    auto [b1, t1] = sample();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto [b2, t2] = sample();
    return (t2 - t1) ? 100.0 * static_cast<double>(b2 - b1) /
                               static_cast<double>(t2 - t1) : 0.0;
}

static void measureRam(double& ram_pct, unsigned long long& used_mb,
                       unsigned long long& total_mb)
{
    int64_t total_bytes = 0;
    size_t len = sizeof(total_bytes);
    sysctlbyname("hw.memsize", &total_bytes, &len, nullptr, 0);
    total_mb = static_cast<unsigned long long>(total_bytes) / (1024 * 1024);

    vm_statistics64_data_t vm_stat{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    host_statistics64(mach_host_self(), HOST_VM_INFO64,
                      (host_info64_t)&vm_stat, &count);
    uint64_t page = static_cast<uint64_t>(getpagesize());
    uint64_t free_bytes = (vm_stat.free_count + vm_stat.inactive_count) * page;
    used_mb = (static_cast<unsigned long long>(total_bytes) - free_bytes) / (1024 * 1024);
    ram_pct = total_bytes ? 100.0 * (1.0 - static_cast<double>(free_bytes) /
                                          static_cast<double>(total_bytes)) : 0.0;
}

#elif defined(PLATFORM_WINDOWS)
static double measureCpu() {
    auto toULL = [](FILETIME ft) -> ULONGLONG {
        return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    FILETIME idle1, kernel1, user1, idle2, kernel2, user2;
    GetSystemTimes(&idle1, &kernel1, &user1);
    Sleep(500);
    GetSystemTimes(&idle2, &kernel2, &user2);
    ULONGLONG idle   = toULL(idle2)   - toULL(idle1);
    ULONGLONG kernel = toULL(kernel2) - toULL(kernel1);
    ULONGLONG user   = toULL(user2)   - toULL(user1);
    ULONGLONG total  = kernel + user;
    return total ? 100.0 * (1.0 - static_cast<double>(idle) /
                                  static_cast<double>(total)) : 0.0;
}

static void measureRam(double& ram_pct, unsigned long long& used_mb,
                       unsigned long long& total_mb)
{
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    total_mb = ms.ullTotalPhys / (1024 * 1024);
    used_mb  = (ms.ullTotalPhys - ms.ullAvailPhys) / (1024 * 1024);
    ram_pct  = static_cast<double>(ms.dwMemoryLoad);
}
#endif

// ---------------------------------------------------------------------------
// Plugin ABI
// ---------------------------------------------------------------------------

extern "C" {

const char* orbis_plugin_name()    { return "cpu_ram"; }
const char* orbis_plugin_version() { return "1.0.0"; }

const char* collect() {
    static std::string result;
    try {
        double cpu_pct = measureCpu();

        double ram_pct = 0.0;
        unsigned long long used_mb = 0, total_mb = 0;
        measureRam(ram_pct, used_mb, total_mb);

        char msg[128];
        snprintf(msg, sizeof(msg), "CPU: %.1f%% RAM: %.1f%%", cpu_pct, ram_pct);

        json entry;
        entry["source"]  = "cpu_ram_plugin";
        entry["level"]   = "INFO";
        entry["message"] = msg;
        entry["metadata"] = {
            {"cpu_percent",  cpu_pct},
            {"ram_percent",  ram_pct},
            {"ram_used_mb",  used_mb},
            {"ram_total_mb", total_mb}
        };

        json arr = json::array();
        arr.push_back(entry);
        result = arr.dump();
    } catch (...) {
        result = "[]";
    }
    return result.c_str();
}

void cleanup() {}

} // extern "C"
