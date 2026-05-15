#include "LocalBuffer.hpp"
#include "constants.hpp"

#include <sqlite3.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

namespace orbis {

// ---------------------------------------------------------------------------
// RAII sqlite3_stmt wrapper
// ---------------------------------------------------------------------------
struct StmtGuard {
    sqlite3_stmt* stmt{nullptr};
    ~StmtGuard() { if (stmt) sqlite3_finalize(stmt); }
};

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

LocalBuffer::LocalBuffer(const std::string& db_path,
                         std::function<bool(const std::string&, const std::string&)> flush_fn)
    : db_path_(db_path), flush_fn_(std::move(flush_fn))
{
    initDb();
}

LocalBuffer::~LocalBuffer() {
    if (db_) sqlite3_close(db_);
}

// ---------------------------------------------------------------------------
// DB initialisation
// ---------------------------------------------------------------------------

void LocalBuffer::initDb() {
    int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("LocalBuffer: cannot open DB at " + db_path_ +
                                 " — " + sqlite3_errmsg(db_));
    }

    const char* ddl =
        "CREATE TABLE IF NOT EXISTS messages ("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  topic     TEXT    NOT NULL,"
        "  payload   TEXT    NOT NULL,"
        "  timestamp INTEGER NOT NULL,"
        "  attempts  INTEGER NOT NULL DEFAULT 0"
        ");";

    char* err = nullptr;
    rc = sqlite3_exec(db_, ddl, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error("LocalBuffer: DDL failed — " + msg);
    }

    // Performance pragmas
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);

    spdlog::info("LocalBuffer initialized at {}", db_path_);
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

void LocalBuffer::store(const std::string& topic, const std::string& payload) {
    std::lock_guard<std::mutex> lock(mu_);

    pruneIfNeeded();

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    const char* sql = "INSERT INTO messages(topic, payload, timestamp, attempts) VALUES(?,?,?,0);";
    StmtGuard sg;
    if (sqlite3_prepare_v2(db_, sql, -1, &sg.stmt, nullptr) != SQLITE_OK) {
        spdlog::error("LocalBuffer: prepare insert failed");
        return;
    }
    sqlite3_bind_text(sg.stmt, 1, topic.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(sg.stmt, 2, payload.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(sg.stmt, 3, static_cast<sqlite3_int64>(now_ms));

    if (sqlite3_step(sg.stmt) != SQLITE_DONE) {
        spdlog::error("LocalBuffer: insert failed: {}", sqlite3_errmsg(db_));
    }
}

// ---------------------------------------------------------------------------
// Flush
// ---------------------------------------------------------------------------

size_t LocalBuffer::flush() {
    size_t flushed = 0;

    while (true) {
        // Fetch next batch
        struct Row { long long id; std::string topic; std::string payload; };
        std::vector<Row> batch;

        {
            std::lock_guard<std::mutex> lock(mu_);
            const char* sql =
                "SELECT id, topic, payload FROM messages ORDER BY id ASC LIMIT ?;";
            StmtGuard sg;
            if (sqlite3_prepare_v2(db_, sql, -1, &sg.stmt, nullptr) != SQLITE_OK) break;
            sqlite3_bind_int(sg.stmt, 1, constants::BUFFER_FLUSH_BATCH_SIZE);
            while (sqlite3_step(sg.stmt) == SQLITE_ROW) {
                Row r;
                r.id      = sqlite3_column_int64(sg.stmt, 0);
                r.topic   = reinterpret_cast<const char*>(sqlite3_column_text(sg.stmt, 1));
                r.payload = reinterpret_cast<const char*>(sqlite3_column_text(sg.stmt, 2));
                batch.push_back(std::move(r));
            }
        }

        if (batch.empty()) break;

        for (auto& row : batch) {
            if (!flush_fn_(row.topic, row.payload)) {
                // Connection lost again — stop flushing
                spdlog::warn("LocalBuffer: flush interrupted (publish failed)");
                return flushed;
            }
            // Delete on success
            {
                std::lock_guard<std::mutex> lock(mu_);
                const char* del = "DELETE FROM messages WHERE id = ?;";
                StmtGuard sg;
                if (sqlite3_prepare_v2(db_, del, -1, &sg.stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int64(sg.stmt, 1, row.id);
                    sqlite3_step(sg.stmt);
                }
            }
            ++flushed;
        }

        // Brief pause between batches to avoid flooding the broker
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (flushed > 0) {
        spdlog::info("LocalBuffer: flushed {} messages", flushed);
    }
    return flushed;
}

// ---------------------------------------------------------------------------
// Size
// ---------------------------------------------------------------------------

size_t LocalBuffer::size() {
    std::lock_guard<std::mutex> lock(mu_);
    const char* sql = "SELECT COUNT(*) FROM messages;";
    StmtGuard sg;
    if (sqlite3_prepare_v2(db_, sql, -1, &sg.stmt, nullptr) != SQLITE_OK) return 0;
    if (sqlite3_step(sg.stmt) == SQLITE_ROW)
        return static_cast<size_t>(sqlite3_column_int64(sg.stmt, 0));
    return 0;
}

// ---------------------------------------------------------------------------
// Prune oldest entries when at capacity (called with mu_ held)
// ---------------------------------------------------------------------------

void LocalBuffer::pruneIfNeeded() {
    const char* count_sql = "SELECT COUNT(*) FROM messages;";
    StmtGuard sg;
    if (sqlite3_prepare_v2(db_, count_sql, -1, &sg.stmt, nullptr) != SQLITE_OK) return;
    if (sqlite3_step(sg.stmt) != SQLITE_ROW) return;
    long long cnt = sqlite3_column_int64(sg.stmt, 0);

    if (cnt >= constants::BUFFER_MAX_MESSAGES) {
        long long to_delete = cnt - constants::BUFFER_MAX_MESSAGES + 1000;
        spdlog::warn("LocalBuffer: at capacity ({}), pruning {} oldest entries",
                     cnt, to_delete);
        const char* del = "DELETE FROM messages WHERE id IN "
                          "(SELECT id FROM messages ORDER BY id ASC LIMIT ?);";
        StmtGuard sg2;
        if (sqlite3_prepare_v2(db_, del, -1, &sg2.stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(sg2.stmt, 1, to_delete);
            sqlite3_step(sg2.stmt);
        }
    }
}

} // namespace orbis
