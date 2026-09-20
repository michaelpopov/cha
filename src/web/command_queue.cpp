#include "web/command_queue.h"

#include <utility>

namespace cha {

bool CommandReply::complete(CommandSubmitResult result) {
    std::function<void()> callback;
    {
        std::lock_guard lock(mutex_);
        if (result_ || abandoned_) return false;
        result_ = std::move(result);
        callback = std::move(ready_callback_);
    }
    ready_.notify_all();
    if (callback) callback();
    return true;
}

std::optional<CommandSubmitResult> CommandReply::wait_for(
    std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    if (!ready_.wait_for(lock, timeout, [this] { return result_.has_value(); })) {
        abandoned_ = true;
        return std::nullopt;
    }
    return result_;
}

void CommandReply::set_ready_callback(std::function<void()> callback) {
    {
        std::lock_guard lock(mutex_);
        if (abandoned_) return;
        if (!result_) {
            ready_callback_ = std::move(callback);
            return;
        }
    }
    if (callback) callback();
}

std::optional<CommandSubmitResult> CommandReply::peek() const {
    std::lock_guard lock(mutex_);
    return result_;
}

void CommandReply::abandon() const {
    std::lock_guard lock(mutex_);
    abandoned_ = true;
    ready_callback_ = {};
}

} // namespace cha
