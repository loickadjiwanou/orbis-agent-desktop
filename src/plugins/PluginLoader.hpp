#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace orbis {

/// @brief Data returned by a single plugin collect() call.
struct PluginLogEntry {
    std::string source;
    std::string level;
    std::string message;
    std::string metadata_json; ///< Raw JSON object string for the "metadata" field
};

/// @brief Handle to a loaded plugin shared library.
struct LoadedPlugin {
    std::string name;
    std::string version;
    void*       handle{nullptr};   ///< dlopen / LoadLibrary handle

    // Function pointers resolved at load time
    const char* (*fn_collect)()  {nullptr};
    void        (*fn_cleanup)()  {nullptr};
};

/**
 * @brief Scans a directory, loads all plugin shared libraries, and periodically
 * calls their collect() function.
 *
 * Plugin ABI (extern "C"):
 *   const char* orbis_plugin_name()    → static identifier, e.g. "cpu_ram"
 *   const char* orbis_plugin_version() → semver string
 *   const char* collect()              → JSON array of log entries (see spec)
 *   void        cleanup()              → free any resources before unload
 *
 * If a plugin throws or returns malformed JSON, it is unloaded without
 * crashing the agent.
 */
class PluginLoader {
public:
    using LogCallback = std::function<void(std::vector<PluginLogEntry>)>;

    explicit PluginLoader(const std::string& plugins_dir);
    ~PluginLoader();

    // Non-copyable
    PluginLoader(const PluginLoader&) = delete;
    PluginLoader& operator=(const PluginLoader&) = delete;

    /// Scan the plugins directory and load all valid plugins.
    void loadAll();

    /// Unload all plugins (calls cleanup() on each).
    void unloadAll();

    /**
     * @brief Call collect() on all loaded plugins and invoke cb with results.
     * Plugins that fail are quietly unloaded.
     */
    void collectAll(const LogCallback& cb);

    size_t pluginCount() const;

private:
    bool loadPlugin(const std::string& path);
    void unloadPlugin(LoadedPlugin& plugin);
    std::vector<PluginLogEntry> parseCollectResult(const std::string& json_str,
                                                    const std::string& plugin_name);

    std::string plugins_dir_;
    mutable std::mutex mu_;
    std::vector<LoadedPlugin> plugins_;
};

} // namespace orbis
