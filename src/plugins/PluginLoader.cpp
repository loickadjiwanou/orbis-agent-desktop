#include "PluginLoader.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <stdexcept>

#if defined(PLATFORM_WINDOWS)
#  include <windows.h>
#  define PLUGIN_EXT ".dll"
#  define dlopen(p,f)  static_cast<void*>(LoadLibraryA(p))
#  define dlsym(h,s)   reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(h), s))
#  define dlclose(h)   FreeLibrary(static_cast<HMODULE>(h))
#  define dlerror()    "LoadLibrary error"
#else
#  include <dlfcn.h>
// CMake MODULE libraries always produce .so on both Linux and macOS
// (.dylib is for SHARED libraries; MODULE/plugin targets use .so on macOS too)
#  define PLUGIN_EXT ".so"
#endif

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace orbis {

PluginLoader::PluginLoader(const std::string& plugins_dir)
    : plugins_dir_(plugins_dir) {}

PluginLoader::~PluginLoader() {
    unloadAll();
}

// ---------------------------------------------------------------------------
// Load all plugins from the directory
// ---------------------------------------------------------------------------

void PluginLoader::loadAll() {
    std::lock_guard<std::mutex> lock(mu_);

    if (!fs::exists(plugins_dir_)) {
        spdlog::warn("Plugins directory does not exist: {}", plugins_dir_);
        return;
    }

    for (auto& entry : fs::directory_iterator(plugins_dir_)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != PLUGIN_EXT) continue;
        loadPlugin(entry.path().string());
    }

    spdlog::info("PluginLoader: {} plugin(s) loaded from {}", plugins_.size(), plugins_dir_);
}

bool PluginLoader::loadPlugin(const std::string& path) {
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        spdlog::warn("Plugin load failed {}: {}", path, dlerror());
        return false;
    }

    using NameFn    = const char*(*)();
    using VersionFn = const char*(*)();
    using CollectFn = const char*(*)();
    using CleanupFn = void(*)();

    auto fn_name    = reinterpret_cast<NameFn>   (dlsym(handle, "orbis_plugin_name"));
    auto fn_version = reinterpret_cast<VersionFn>(dlsym(handle, "orbis_plugin_version"));
    auto fn_collect = reinterpret_cast<CollectFn>(dlsym(handle, "collect"));
    auto fn_cleanup = reinterpret_cast<CleanupFn>(dlsym(handle, "cleanup"));

    if (!fn_name || !fn_version || !fn_collect || !fn_cleanup) {
        spdlog::warn("Plugin missing required symbols: {}", path);
        dlclose(handle);
        return false;
    }

    LoadedPlugin p;
    p.name       = fn_name();
    p.version    = fn_version();
    p.handle     = handle;
    p.fn_collect = fn_collect;
    p.fn_cleanup = fn_cleanup;

    plugins_.push_back(std::move(p));
    spdlog::info("Plugin loaded: {} v{}", plugins_.back().name, plugins_.back().version);
    return true;
}

// ---------------------------------------------------------------------------
// Unload
// ---------------------------------------------------------------------------

void PluginLoader::unloadAll() {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& p : plugins_) unloadPlugin(p);
    plugins_.clear();
}

void PluginLoader::unloadPlugin(LoadedPlugin& plugin) {
    if (plugin.fn_cleanup) {
        try { plugin.fn_cleanup(); } catch (...) {}
    }
    if (plugin.handle) {
        dlclose(plugin.handle);
        plugin.handle = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Collect
// ---------------------------------------------------------------------------

void PluginLoader::collectAll(const LogCallback& cb) {
    std::lock_guard<std::mutex> lock(mu_);

    std::vector<size_t> to_remove;

    for (size_t i = 0; i < plugins_.size(); ++i) {
        auto& p = plugins_[i];
        try {
            const char* raw = p.fn_collect();
            if (!raw) continue;
            auto entries = parseCollectResult(raw, p.name);
            if (!entries.empty()) cb(std::move(entries));
        } catch (const std::exception& e) {
            spdlog::error("Plugin '{}' collect() threw: {} — unloading", p.name, e.what());
            to_remove.push_back(i);
        } catch (...) {
            spdlog::error("Plugin '{}' collect() threw unknown exception — unloading", p.name);
            to_remove.push_back(i);
        }
    }

    // Unload failed plugins (reverse order to preserve indices)
    for (auto it = to_remove.rbegin(); it != to_remove.rend(); ++it) {
        unloadPlugin(plugins_[*it]);
        plugins_.erase(plugins_.begin() + static_cast<ptrdiff_t>(*it));
    }
}

// ---------------------------------------------------------------------------
// Parse collect() return value
// ---------------------------------------------------------------------------

std::vector<PluginLogEntry> PluginLoader::parseCollectResult(const std::string& json_str,
                                                              const std::string& plugin_name)
{
    std::vector<PluginLogEntry> entries;
    if (json_str.empty()) return entries;

    try {
        auto arr = json::parse(json_str);
        if (!arr.is_array()) return entries;

        for (auto& item : arr) {
            PluginLogEntry e;
            e.source        = item.value("source",  plugin_name);
            e.level         = item.value("level",   "INFO");
            e.message       = item.value("message", "");
            e.metadata_json = item.contains("metadata")
                              ? item["metadata"].dump()
                              : "{}";
            entries.push_back(std::move(e));
        }
    } catch (const json::parse_error& ex) {
        spdlog::error("Plugin '{}' returned invalid JSON: {}", plugin_name, ex.what());
    }
    return entries;
}

size_t PluginLoader::pluginCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return plugins_.size();
}

} // namespace orbis
