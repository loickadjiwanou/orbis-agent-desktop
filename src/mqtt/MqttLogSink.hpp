#pragma once
#include <spdlog/sinks/base_sink.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>

using PublishFn = std::function<bool(const std::string&, const std::string&, int, bool)>;

/**
 * spdlog sink that forwards log records to the MQTT broker as JSON.
 * Topic: devices/{device_id}/agent_logs
 * QoS 0, not retained (fire-and-forget — we don't want logs to accumulate on broker).
 * Only forwards INFO and above to avoid flooding; DEBUG is local-only.
 * Has a simple recursion guard so that publishing itself never loops.
 */
template<typename Mutex>
class MqttLogSinkT : public spdlog::sinks::base_sink<Mutex> {
public:
    MqttLogSinkT(PublishFn publish_fn, std::string device_id, std::string topic)
        : publish_fn_(std::move(publish_fn))
        , device_id_(std::move(device_id))
        , topic_(std::move(topic))
    {}

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        // Recursion guard
        if (in_publish_) return;
        in_publish_ = true;

        try {
            const char* level_str = levelToString(msg.level);

            auto now_t = std::chrono::system_clock::to_time_t(msg.time);
            char ts[32] = {};
            struct tm tm_buf{};
#if defined(PLATFORM_WINDOWS)
            gmtime_s(&tm_buf, &now_t);
#else
            gmtime_r(&now_t, &tm_buf);
#endif
            std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);

            std::string source = (msg.logger_name.size() == 0)
                ? "agent"
                : std::string(msg.logger_name.data(), msg.logger_name.size());

            nlohmann::json j;
            j["device_id"] = device_id_;
            j["timestamp"] = std::string(ts);
            j["level"]     = level_str;
            j["source"]    = source;
            j["message"]   = std::string(msg.payload.data(), msg.payload.size());

            publish_fn_(topic_, j.dump(), 0, false);
        } catch (...) {
            // Never throw from a sink
        }

        in_publish_ = false;
    }

    void flush_() override {}

private:
    static const char* levelToString(spdlog::level::level_enum lvl) {
        switch (lvl) {
            case spdlog::level::trace:
            case spdlog::level::debug:    return "DEBUG";
            case spdlog::level::info:     return "INFO";
            case spdlog::level::warn:     return "WARNING";
            case spdlog::level::err:      return "ERROR";
            case spdlog::level::critical: return "CRITICAL";
            default:                      return "INFO";
        }
    }

    PublishFn   publish_fn_;
    std::string device_id_;
    std::string topic_;
    bool        in_publish_ = false;
};

using MqttLogSink = MqttLogSinkT<std::mutex>;
