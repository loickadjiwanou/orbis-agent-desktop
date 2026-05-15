#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <string>

struct sqlite3;

namespace orbis {

/**
 * @brief SQLite-backed persistent buffer for MQTT messages that could not be delivered.
 *
 * Messages are stored in a local SQLite database and flushed automatically
 * when the MQTT connection is restored.  Survives process restarts.
 * Capped at constants::BUFFER_MAX_MESSAGES; the oldest entries are pruned first.
 */
class LocalBuffer {
public:
    /**
     * @param db_path   Absolute path to the SQLite database file.
     * @param flush_fn  Called during flush for each buffered message.
     *                  Returns true if the message was published successfully.
     */
    explicit LocalBuffer(const std::string& db_path,
                         std::function<bool(const std::string& topic,
                                            const std::string& payload)> flush_fn);
    ~LocalBuffer();

    // Non-copyable
    LocalBuffer(const LocalBuffer&) = delete;
    LocalBuffer& operator=(const LocalBuffer&) = delete;

    /// Store a message for later delivery.
    void store(const std::string& topic, const std::string& payload);

    /**
     * @brief Attempt to publish all stored messages using flush_fn.
     * Processes in batches of constants::BUFFER_FLUSH_BATCH_SIZE.
     * Stops if flush_fn returns false (connection lost again).
     * @return Number of messages successfully flushed.
     */
    size_t flush();

    /// Number of messages currently buffered.
    size_t size();

private:
    void initDb();
    void pruneIfNeeded();

    std::string db_path_;
    std::function<bool(const std::string&, const std::string&)> flush_fn_;

    mutable std::mutex mu_;
    sqlite3* db_{nullptr};
};

} // namespace orbis
