#include "MessageQueue.hpp"
#include <spdlog/spdlog.h>

namespace orbis {

MessageQueue::MessageQueue(size_t max_size) : max_size_(max_size) {}

void MessageQueue::push(Message msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= max_size_) {
        spdlog::warn("MessageQueue full ({} items), dropping message on topic: {}",
                     max_size_, msg.topic);
        return;
    }
    queue_.push(std::move(msg));
    cv_.notify_one();
}

bool MessageQueue::pop(Message& msg, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    bool ok = cv_.wait_for(lock, timeout, [this] {
        return !queue_.empty() || shutdown_;
    });
    if (!ok || queue_.empty()) return false;
    msg = std::move(queue_.front());
    queue_.pop();
    return true;
}

size_t MessageQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void MessageQueue::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_ = true;
    cv_.notify_all();
}

} // namespace orbis
