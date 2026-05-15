/**
 * @file disk.cpp
 * @brief Orbis plugin — disk usage per mount point.
 *
 * Linux/macOS: statvfs on the main mount points.
 * Windows    : GetLogicalDrives + GetDiskFreeSpaceEx.
 */
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#if defined(PLATFORM_WINDOWS)
#  include <windows.h>
#else
#  include <sys/statvfs.h>
#  include <fstream>
#endif

using json = nlohmann::json;

struct DiskInfo {
    std::string mount;
    unsigned long long total_gb;
    unsigned long long used_gb;
    double percent;
};

static std::vector<DiskInfo> gatherDisks() {
    std::vector<DiskInfo> disks;

#if defined(PLATFORM_LINUX)
    // Parse /proc/mounts to get real mount points
    std::ifstream f("/proc/mounts");
    std::string dev, mp, fstype, opts;
    int dump, pass;
    while (f >> dev >> mp >> fstype >> opts >> dump >> pass) {
        // Only physical-looking fs types
        if (fstype == "tmpfs" || fstype == "devtmpfs" || fstype == "sysfs" ||
            fstype == "proc"  || fstype == "cgroup"   || fstype == "devpts" ||
            fstype == "securityfs" || fstype == "pstore") continue;
        struct statvfs sv{};
        if (statvfs(mp.c_str(), &sv) != 0 || sv.f_blocks == 0) continue;
        DiskInfo d;
        d.mount    = mp;
        d.total_gb = (sv.f_blocks * sv.f_frsize) / (1024ULL * 1024 * 1024);
        d.used_gb  = ((sv.f_blocks - sv.f_bfree) * sv.f_frsize) / (1024ULL * 1024 * 1024);
        d.percent  = 100.0 * (1.0 - static_cast<double>(sv.f_bfree) /
                                    static_cast<double>(sv.f_blocks));
        disks.push_back(d);
        if (disks.size() >= 8) break; // cap at 8 mount points
    }

#elif defined(PLATFORM_MACOS)
    for (auto* mp : {"/", "/System/Volumes/Data"}) {
        struct statvfs sv{};
        if (statvfs(mp, &sv) != 0 || sv.f_blocks == 0) continue;
        DiskInfo d;
        d.mount    = mp;
        d.total_gb = (sv.f_blocks * sv.f_frsize) / (1024ULL * 1024 * 1024);
        d.used_gb  = ((sv.f_blocks - sv.f_bfree) * sv.f_frsize) / (1024ULL * 1024 * 1024);
        d.percent  = 100.0 * (1.0 - static_cast<double>(sv.f_bfree) /
                                    static_cast<double>(sv.f_blocks));
        disks.push_back(d);
    }

#elif defined(PLATFORM_WINDOWS)
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1 << i))) continue;
        char root[4] = {'A' + static_cast<char>(i), ':', '\\', '\0'};
        if (GetDriveTypeA(root) != DRIVE_FIXED) continue;
        ULARGE_INTEGER free_b, total_b, total_free;
        if (!GetDiskFreeSpaceExA(root, &free_b, &total_b, &total_free)) continue;
        if (total_b.QuadPart == 0) continue;
        DiskInfo d;
        d.mount    = root;
        d.total_gb = total_b.QuadPart / (1024ULL * 1024 * 1024);
        d.used_gb  = (total_b.QuadPart - free_b.QuadPart) / (1024ULL * 1024 * 1024);
        d.percent  = 100.0 * (1.0 - static_cast<double>(free_b.QuadPart) /
                                    static_cast<double>(total_b.QuadPart));
        disks.push_back(d);
    }
#endif
    return disks;
}

// ---------------------------------------------------------------------------
// Plugin ABI
// ---------------------------------------------------------------------------

extern "C" {

const char* orbis_plugin_name()    { return "disk"; }
const char* orbis_plugin_version() { return "1.0.0"; }

const char* collect() {
    static std::string result;
    try {
        auto disks = gatherDisks();
        json arr = json::array();

        for (auto& d : disks) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Disk %s: %.1f%% used (%llu/%llu GB)",
                     d.mount.c_str(), d.percent,
                     static_cast<unsigned long long>(d.used_gb),
                     static_cast<unsigned long long>(d.total_gb));

            std::string level = (d.percent >= 90.0) ? "ERROR"
                              : (d.percent >= 75.0) ? "WARNING"
                              : "INFO";
            json entry;
            entry["source"]  = "disk_plugin";
            entry["level"]   = level;
            entry["message"] = msg;
            entry["metadata"] = {
                {"mount_point", d.mount},
                {"total_gb",    d.total_gb},
                {"used_gb",     d.used_gb},
                {"percent",     d.percent}
            };
            arr.push_back(entry);
        }
        result = arr.dump();
    } catch (...) {
        result = "[]";
    }
    return result.c_str();
}

void cleanup() {}

} // extern "C"
