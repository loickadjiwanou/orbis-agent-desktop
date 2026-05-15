#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>

namespace orbis {

/// @brief A single inbound MQTT message.
struct Message {
    std::string topic;
    std::string payload;
    std::chrono::system_clock::time_point received_at;
};

/**
 * @brief Thread-safe, bounded FIFO queue for inbound MQTT messages.
 *
 * The MQTT callback thread pushes, service threads pop.
 * pop() blocks until a message arrives or the timeout elapses.
 */
class MessageQueue {
public:
    explicit MessageQueue(size_t max_size = 10000);

    /// Push a message. Drops silently if the queue is full (backpressure).
    void push(Message msg);

    /**
     * @brief Pop the next message, blocking up to @p timeout.
     * @param[out] msg   Populated on success.
     * @param timeout    Maximum wait duration.
     * @return true if a message was returned, false on timeout.
     */
    bool pop(Message& msg, std::chrono::milliseconds timeout);

    /// Number of messages currently in the queue.
    size_t size() const;

    /// Signal all waiting threads to unblock (used during shutdown).
    void shutdown();

private:
    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    std::queue<Message>     queue_;
    size_t                  max_size_;
    bool                    shutdown_{false};
};

} // namespace orbis
